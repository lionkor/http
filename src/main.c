#include "http_server.h"
#include "logging.h"
#include "memory.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <unistd.h>

size_t requests_handled = 0;

static void serve_404(http_client* client, const http_header_data* template_hdr_data) {
    error_t err;
    http_header_data this_hdr = *template_hdr_data;
    this_hdr.content_type = "text/html";
    this_hdr.status_code = 404;
    this_hdr.status_message = "Not Found";
    http_client_serve(client, http_server_err_404_page, http_server_err_404_page_size, &this_hdr, &err);
    if (is_error(err)) {
        print_error(err);
    }
}

static size_t min_size_t(size_t a, size_t b) {
    return a < b ? a : b;
}

#define KB 1024
#define MB KB * 1024
#define GB MB * 1024

typedef struct {
    char* data;
    size_t len;
} char_buffer_t;

static char_buffer_t build_directory_buffer(const char* path, error_t* ep) {
    *ep = new_error_ok();
    size_t buf_size = 16 * KB;
    char* buf = safe_malloc(buf_size, ep);
    if (is_error(*ep)) {
        return (char_buffer_t) { NULL, 0 };
    }
    memset(buf, 0, buf_size);
    DIR* dir = opendir(path);
    if (!dir) {
        perror("opendir");
        *ep = new_error_error("opendir() failed");
        return (char_buffer_t) { NULL, 0 };
    }
    struct dirent* folder = NULL;
    size_t written = 0;
    size_t n = 0;
    do {
        errno = 0;
        folder = readdir(dir);
        if (folder) {
            // only consider directories and regular files
            if (folder->d_type == DT_DIR || folder->d_type == DT_REG) {
                char line[1 * KB];
                memset(line, 0, sizeof(line));
                n = sprintf(line, "<a href=\"./%s\">%s</a>", folder->d_name, folder->d_name);
                memcpy(buf + written, line, min_size_t(strlen(folder->d_name), n));
                written += n;
            }
        } else {
            if (errno != 0) {
                perror("readdir");
                log_warning("failed to read an entry from '%s'", path);
            }
        }
    } while (folder && n < buf_size);
    closedir(dir);
    return (char_buffer_t) { buf, written };
}

void handle_client_request(http_server* server, http_client* client) {
    log_info("%s: %p", "got client", (void*)client);

    bool keep_alive = false;

    error_t err = new_error_ok();

    http_client_set_rcv_timeout(client, 5, 0, &err);
    if (is_error(err)) {
        print_error(err);
        log_error("%s", "socket will not timeout on rcv, high risk of locking up, aborting connection");
        goto shutdown_and_free;
    }

    http_header_data hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.additional_headers = "Keep-Alive: timeout=5" CRLF
                             "Server: lionkor/http" CRLF;
    hdr.connection = "close";
    hdr.status_code = 200;
    hdr.status_message = "OK";

    do {
        err = new_error_ok();
        log_info("%s", "receiving and parsing header");
        http_header header;

        http_client_receive_header(client, &header, &err);
        if (is_error(err)) {
            print_error(err);
            log_error("%s", "request failed");
            // this could be a timeout or the client dropping,
            // so we just cancel the keep-alive here
            break;
        }

        char buf[128];
        http_header_parse_field(&header, buf, sizeof(buf), "Connection", &err);
        if (is_error(err)) {
            print_error(err);
        } else {
            if (strcmp(buf, "keep-alive") == 0) {
                log_info("%s", "keep alive");
                keep_alive = true;
                hdr.connection = "keep-alive";
            } else {
                log_info("%s", "don't keep alive");
                hdr.connection = "close";
            }
        }

        if (strcmp(header.method, "GET") == 0) {
            if (server->show_root_page && strcmp(header.target, "/") == 0) {
                http_header_data this_hdr = hdr;
                this_hdr.content_type = "text/html";
                http_client_serve(client, http_server_rootpage,
                    http_server_rootpage_size, &this_hdr, &err);
                if (is_error(err)) {
                    print_error(err);
                }
            } else if (header.target[0] == '/') {
                // it's a path! resolve and serve
                const char* rel_path = header.target + 1;
                // validate path is a subpath of our root
                char full_rel_path[256];
                memset(full_rel_path, 0, sizeof(full_rel_path));
                memcpy(full_rel_path, server->cwd, min_size_t(sizeof(server->cwd), sizeof(full_rel_path)));
                strncat(full_rel_path, "/", sizeof(full_rel_path) - strlen(full_rel_path) - 1);
                strncat(full_rel_path, rel_path, sizeof(full_rel_path) - strlen(full_rel_path) - 1);
                log_info("checking if '%s' is under '%s'", full_rel_path, server->cwd);
                if (strncmp(full_rel_path, server->cwd, strlen(server->cwd)) != 0) {
                    log_error("attempt to access '%s', which isn't inside '%s' (forbidden)", rel_path, server->cwd);
                    http_header_data this_hdr = hdr;
                    this_hdr.content_type = "text/html";
                    this_hdr.status_code = 403;
                    this_hdr.status_message = "Forbidden";
                    http_client_serve(client, http_server_err_403_page,
                        http_server_err_403_page_size, &this_hdr, &err);
                    if (is_error(err)) {
                        print_error(err);
                    }
                    continue;
                }
                struct stat st;
                int ret = stat(full_rel_path, &st);
                if (ret < 0) {
                    log_error("couldn't stat '%s'", full_rel_path);
                    serve_404(client, &hdr);
                    continue;
                }
                if (S_ISDIR(st.st_mode)) {
                    // serve directory
                    char_buffer_t buf = build_directory_buffer(full_rel_path, &err);
                    if (is_error(err)) {
                        print_error(err);
                        http_header_data this_hdr = hdr;
                        this_hdr.content_type = "text/html";
                        this_hdr.status_code = 500;
                        this_hdr.status_message = "Internal Server Error";
                        http_client_serve(client, http_server_err_500_page,
                            http_server_err_500_page_size, &this_hdr, &err);
                        if (is_error(err)) {
                            print_error(err);
                        }
                        free(buf.data);
                        continue;
                    }
                    http_header_data this_hdr = hdr;
                    this_hdr.content_type = "text/html";
                    http_client_serve(client, buf.data, buf.len, &this_hdr, &err);
                    free(buf.data);
                } else {
                    FILE* file = fopen(full_rel_path, "r");
                    if (!file) {
                        log_error("couldn't open '%s'", full_rel_path);
                        perror("fopen");
                        serve_404(client, &hdr);
                        continue;
                    }
                    char* malloced_buf = (char*)safe_malloc(st.st_size, &err);
                    if (is_error(err)) {
                        print_error(err);
                        fclose(file);
                        continue;
                    }
                    size_t n = fread(malloced_buf, 1, st.st_size, file);
                    fclose(file);
                    http_header_data this_hdr = hdr;
                    this_hdr.content_type = "text/html";
                    http_client_serve(client, malloced_buf, n, &this_hdr, &err);
                    free(malloced_buf);
                    if (is_error(err)) {
                        print_error(err);
                    }
                    if (n != (size_t)st.st_size) {
                        log_warning("read %llu, expected to read %llu", (unsigned long long)n, (unsigned long long)st.st_size);
                    }
                }
            } else {
                serve_404(client, &hdr);
                continue;
            }
        }
        ++requests_handled;
        if (requests_handled % 1000 == 0) {
            fprintf(stderr, "requests handled: %llu\n", (unsigned long long)requests_handled);
        }
    } while (keep_alive);

shutdown_and_free:
    shutdown(client->socket, SHUT_RD);
    close(client->socket);
    free(client);
}

void handle_signals(int sig) {
    switch (sig) {
    case SIGINT:
        exit(2);
        break;
    }
}

int main(void) {
    signal(SIGINT, handle_signals);
    log_info("%s", "welcome to http-server 1.0");
    error_t err = new_error_ok();
    http_server* server = new_http_server(&err);
    if (is_error(err)) {
        print_error(err);
        return __LINE__;
    }
    server->backlog = 10;
    server->show_root_page = false;
    start_http_server(server, 11000, &err);
    if (is_error(err)) {
        print_error(err);
        return __LINE__;
    }
    for (;;) {
        http_server_accept_client(server, handle_client_request, &err);
        if (is_error(err)) {
            print_error(err);
        }
    }
    free_http_server(server);
    log_info("%s", "http-server terminated");
}
