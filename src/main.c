#include "http_server.h"
#include "logging.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <unistd.h>

size_t requests_handled = 0;

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
            } else {
                log_info("%s", "don't keep alive");
            }
        }

        if (strcmp(header.method, "GET") == 0) {
            if (strcmp(header.target, "/") == 0) {
                char buf[] = "HELLO, WORLD!";
                http_client_serve(client, buf, sizeof(buf) - 1, "text/plain", &err);
                if (is_error(err)) {
                    print_error(err);
                }
            } else if (header.target[0] == '/') {
                // it's a path! resolve and serve
                const char* rel_path = header.target + 1;
                // validate path is a subpath of our root
                char full_path[128];
                char* res = realpath(rel_path, full_path);
                if (res == NULL) {
                    perror("realpath");
                    http_server_serve_error_page(client, 404, &err);
                    continue;
                }
                log_info("resolved '%s' to '%s'", rel_path, full_path);
                if (strncmp(full_path, server->cwd, strlen(server->cwd)) != 0) {
                    log_error("attempt to access '%s', which isn't inside '%s' (forbidden)", full_path, server->cwd);
                    http_server_serve_error_page(client, 403, &err);
                    continue;
                }
                FILE* file = fopen(full_path, "r");
                if (!file) {
                    log_error("couldn't open '%s'", full_path);
                    perror("fopen");
                    http_server_serve_error_page(client, 404, &err);
                    continue;
                }
                struct stat st;
                int ret = stat(full_path, &st);
                if (ret < 0) {
                    log_error("couldn't stat '%s'", full_path);
                    http_server_serve_error_page(client, 404, &err);
                    if (is_error(err)) {
                        print_error(err);
                    }
                    fclose(file);
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
                http_client_serve(client, malloced_buf, n, "text/html", &err);
                free(malloced_buf);
                if (is_error(err)) {
                    print_error(err);
                }
                if (n != (size_t)st.st_size) {
                    log_warning("read %llu, expected to read %llu", (unsigned long long)n, (unsigned long long)st.st_size);
                }
            } else {
                http_server_serve_error_page(client, 404, &err);
                if (is_error(err)) {
                    print_error(err);
                }
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
