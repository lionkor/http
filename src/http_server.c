#include "http_server.h"

#include "logging.h"
#include "memory.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

http_server* http_server_new(http_error_t* ep) {
    *ep = new_error_ok();
    http_server* server = (http_server*)safe_malloc(sizeof(http_server), ep);
    if (is_error(*ep)) {
        print_error(*ep);
    }
    server->socket = 0;
    server->backlog = 1;
    if (getcwd(server->cwd, sizeof(server->cwd)) == NULL) {
        *ep = new_error_error("getcwd() failed, server's cwd is not set");
    }
    return server;
}

void http_server_free(http_server* server) {
    free(server);
}

static void handle_sigalarm(int sig) {
    (void)sig;
    // do nothing
}

void http_server_start(http_server* server, uint16_t port, http_error_t* ep) {
    assert(server);
    *ep = new_error_ok();
    server->socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server->socket == -1) {
        perror("socket");
        *ep = new_error_error("socket() failed");
        return;
    }
    log_info("%s", "socket created");
    struct sockaddr_in address;
    memset(&address, 0, sizeof(struct sockaddr_in));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    int flag = 1;
    if (setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0) {
        perror("setsockopt");
        log_warning("%s", "failed to set SO_REUSEADDR");
    }
    int ret = bind(server->socket, (struct sockaddr*)&address, sizeof(address));
    if (ret != 0) {
        perror("bind");
        *ep = new_error_error("bind() failed");
        return;
    }
    log_info("socket bound to port %d", port);
    ret = listen(server->socket, server->backlog);
    if (ret != 0) {
        perror("bind");
        *ep = new_error_error("listen() failed");
        return;
    }
    log_info("listening on port %d", port);
}

void http_server_accept_client(http_server* server, http_client_connect_cb on_connect, http_error_t* ep) {
    assert(server);
    *ep = new_error_ok();
    http_client* client = (http_client*)safe_malloc(sizeof(http_client), ep);
    memset(client, 0, sizeof(http_client));
    if (is_error(*ep)) {
        return;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(server->socket, &set);
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    int select_ret = select(server->socket + 1, &set, NULL, NULL, NULL);
    if (select_ret == 0) {
        // timed out
        *ep = new_error_error("server socket timed out for accept()");
        free(client);
        return;
    } else if (select_ret < 0) {
        perror("select");
        *ep = new_error_error("select() failed");
        free(client);
        return;
    }
    client->socket = accept(server->socket, (struct sockaddr*)&client->address, &client->address_len);
    if (client->socket < 0) {
        perror("accept");
        *ep = new_error_error("accept() failed");
        free(client);
        return;
    }
    // all good
    log_info("new client accepted, fd %d", client->socket);
    on_connect(server, client);
}

// index of `what`, or `-1` if none found
static int find_next_in_buffer(char* buf, size_t size, char what) {
    for (int i = 0; (size_t)i < size; ++i) {
        if (buf[i] == what) {
            return (int)i;
        }
    }
    return -1;
}

static int find_next_crlf_in_buffer(char* buf, size_t size) {
    for (int i = 0; (size_t)i < size; ++i) {
        if (buf[i] == '\r' && (size_t)i + 1 < size && buf[i + 1] == '\n') {
            return (int)i;
        }
    }
    return -1;
}

void http_client_receive_header(http_client* client, http_header* header, http_error_t* ep) {
    assert(client);
    assert(header);
    *ep = new_error_ok();

    memset(header, 0, sizeof(*header));

    errno = 0;
    ssize_t n = read(client->socket, header->buffer, HTTP_HEADER_SIZE_MAX);
    if (n < 0 || errno != 0) {
        perror("read");
        *ep = new_error_error("read() failed");
        return;
    }
    if (n < 3) {
        // can't be a valid one
        log_error("%s", "got an invalid header (size < 3)");
        *ep = new_error_error("invalid header");
        return;
    }
    header->buffer[n] = '\0';
    //log_info("header: \nHEADER_START\n%s\nHEADER_END", header->buffer);
    char* ptr = header->buffer;

    // SPACE separated header fields
    enum {
        FIRST_HDR_METHOD,
        FIRST_HDR_TARGET,
        FIRST_HDR_SIZEOF
    };

    char* first_errors[FIRST_HDR_SIZEOF];
    first_errors[FIRST_HDR_METHOD] = "failed to parse METHOD";
    first_errors[FIRST_HDR_TARGET] = "failed to parse TARGET";

    char* first_buffers[FIRST_HDR_SIZEOF];
    first_buffers[FIRST_HDR_METHOD] = header->method;
    first_buffers[FIRST_HDR_TARGET] = header->target;

    for (size_t i = 0; i < FIRST_HDR_SIZEOF; ++i) {
        int index = find_next_in_buffer(ptr, n, ' ');
        if (index < 0) {
            *ep = new_error_error(first_errors[i]);
            return;
        }
        memcpy(first_buffers[i], ptr, index);
        first_buffers[i][index] = 0;
        ptr += index + 1;
        //log_info("parsed: '%s'", first_buffers[i]);
    }

    int index = find_next_crlf_in_buffer(ptr, n);
    if (index < 0) {
        *ep = new_error_error("failed to parse VERSION");
        return;
    }
    memcpy(header->version, ptr, index);
    header->version[index] = 0;
    //log_info("parsed: '%s'", header->version);
    header->start_of_headers = index + 2; // crlf

    // parse Host, which is mandatory on HTTP/1.1
    http_header_parse_field(header, header->host, sizeof(header->host), "Host", ep);
    if (is_error(*ep)) {
        return;
    }
    //log_info("parsed: '%s'", header->host);
}

ssize_t http_search_for_string(const char* in, size_t in_size, const char* what, size_t what_size) {
    for (size_t i = 0; i < in_size; ++i) {
        bool match = true;
        for (size_t k = 0; k < what_size; ++k) {
            if (i + k >= in_size || in[i + k] != what[k]) {
                match = false;
                break;
            }
        }
        if (match) {
            return (ssize_t)i;
        }
    }
    return -1;
}

void http_header_parse_field(http_header* header, char* value_buf, size_t value_buf_size, const char* fieldname, http_error_t* ep) {
    char* buf = header->buffer + header->start_of_headers;
    size_t buf_len = HTTP_HEADER_SIZE_MAX - header->start_of_headers;
    ssize_t index = http_search_for_string(buf, buf_len, fieldname, strlen(fieldname));
    if (index < 0) {
        log_info("field %s not found", fieldname);
        *ep = new_error_error("field not found");
        return;
    }
    ssize_t next_colon = find_next_in_buffer(buf + index, buf_len - index, ':');
    if (next_colon < 0) {
        *ep = new_error_error("fieldname found, but not followed by colon");
        return;
    }
    // skip colon
    next_colon++;
    // skip whitespace, if there is any
    if (buf[index + next_colon] == ' ') {
        next_colon++;
    }
    ssize_t next_crlf = find_next_crlf_in_buffer(buf + index, buf_len - index);
    if (next_crlf > -1) {
        if (next_crlf > -1 && next_crlf < next_colon) {
            *ep = new_error_error("fieldname found, but followed by crlf before colon");
            return;
        }
    } else {
        // TODO: this isn't necessary, right? since we end with crlf+crlf
        next_crlf = buf_len;
    }
    // copy value into buffer now

    size_t result_len = next_crlf - next_colon;
    if (value_buf_size < result_len) {
        *ep = new_error_error("buffer too small to fit value of field");
        return;
    }

    // copy everything between colon and crlf
    memset(value_buf, 0, value_buf_size);
    memcpy(value_buf, buf + index + next_colon, result_len);
}

void http_client_serve(http_client* client, const char* body, size_t body_size, http_header_data* header_data, http_error_t* ep) {
    *ep = new_error_ok();
    char header[HTTP_HEADER_SIZE_MAX];
    memset(header, 0, sizeof(header));
    const char header_fmt[] = "HTTP/1.1 %d %s" CRLF
                              "Connection: %s" CRLF
                              "Content-Type: %s" CRLF
                              "Content-Length: %zu" CRLF
                              "%s" CRLF;
    size_t header_size = sprintf(header, header_fmt,
        header_data->status_code,
        header_data->status_message,
        header_data->connection,
        header_data->content_type,
        body_size,
        header_data->additional_headers);

    // allocate buffer for entire response
    size_t response_size = body_size + header_size;
    char* response = safe_malloc(response_size * sizeof(char), ep);
    if (is_error(*ep)) {
        return;
    }
    memcpy(response, header, header_size);
    memcpy(response + header_size, body, body_size);
    ssize_t written = write(client->socket, response, response_size);
    free(response);
    if (written < 0) {
        perror("write");
        *ep = new_error_error("write() failed");
        return;
    }
}

void http_client_set_rcv_timeout(http_client* client, time_t seconds, suseconds_t microseconds, http_error_t* ep) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = microseconds;
    *ep = new_error_ok();
    if (setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        *ep = new_error_error("failed to set rcv timeout");
        perror("setsockopt");
    }
}

void http_client_serve_404(http_client* client, const http_header_data* template_hdr_data, http_error_t* ep) {
    *ep = new_error_ok();
    http_header_data this_hdr = *template_hdr_data;
    this_hdr.content_type = "text/html";
    this_hdr.status_code = 404;
    this_hdr.status_message = "Not Found";
    http_client_serve(client, http_server_err_404_page, http_server_err_404_page_size, &this_hdr, ep);
}

void http_client_serve_403(http_client* client, const http_header_data* template_hdr_data, http_error_t* ep) {
    *ep = new_error_ok();
    http_header_data this_hdr = *template_hdr_data;
    this_hdr.content_type = "text/html";
    this_hdr.status_code = 403;
    this_hdr.status_message = "Forbidden";
    http_client_serve(client, http_server_err_403_page, http_server_err_403_page_size, &this_hdr, ep);
}

void http_client_serve_500(http_client* client, const http_header_data* template_hdr_data, http_error_t* ep) {
    *ep = new_error_ok();
    http_header_data this_hdr = *template_hdr_data;
    this_hdr.content_type = "text/html";
    this_hdr.status_code = 500;
    this_hdr.status_message = "Internal Server Error";
    http_client_serve(client, http_server_err_500_page, http_server_err_500_page_size, &this_hdr, ep);
}

static size_t min_size_t(size_t a, size_t b) {
    return a < b ? a : b;
}

static http_char_buffer_t build_directory_buffer(const char* path, http_error_t* ep) {
    *ep = new_error_ok();
    size_t buf_size = 16 * HTTP_KB;
    char* buf = safe_malloc(buf_size, ep);
    if (is_error(*ep)) {
        return (http_char_buffer_t) { NULL, 0 };
    }
    memset(buf, 0, buf_size);
    DIR* dir = opendir(path);
    if (!dir) {
        perror("opendir");
        *ep = new_error_error("opendir() failed");
        return (http_char_buffer_t) { NULL, 0 };
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
                char line[1 * HTTP_KB];
                memset(line, 0, sizeof(line));
                const char* maybe_slash = "";
                if (folder->d_type == DT_DIR) {
                    maybe_slash = "/";
                }
                n = sprintf(line, "<li><a href=\"%s%s\">%s</a></li>", folder->d_name, maybe_slash, folder->d_name);
                if (strlen(line) < buf_size - written) {
                    size_t grow_by = 16 * HTTP_KB;
                    char* new_buf = realloc(buf, buf_size + grow_by);
                    if (!new_buf) {
                        perror("realloc");
                        *ep = new_error_error("out of memory (realloc)");
                        free(buf);
                        return (http_char_buffer_t) { NULL, 0 };
                    }
                    buf = new_buf;
                    memset(new_buf + buf_size, 0, grow_by);
                    buf_size += grow_by;
                }
                memcpy(buf + written, line, strlen(line));
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
    return (http_char_buffer_t) { buf, written };
}

static const char* get_path_extension(const char* filename) {
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename)
        return "";
    return dot + 1;
}

void http_client_serve_file(http_client* client, http_server* server, const char* target, const http_header_data* hdr, http_error_t* ep) {
    const char* rel_path = target;
    // validate path is a subpath of our root
    char full_rel_path[256];
    memset(full_rel_path, 0, sizeof(full_rel_path));
    memcpy(full_rel_path, server->cwd, min_size_t(sizeof(server->cwd), sizeof(full_rel_path)));
    strncat(full_rel_path, "/", sizeof(full_rel_path) - strlen(full_rel_path) - 1);
    strncat(full_rel_path, rel_path, sizeof(full_rel_path) - strlen(full_rel_path) - 1);
    log_info("checking if '%s' is under '%s'", full_rel_path, server->cwd);
    char resolved[256];
    memset(resolved, 0, sizeof(resolved));
    realpath(full_rel_path, resolved);
    if (strncmp(resolved, server->cwd, strlen(server->cwd)) != 0) {
        log_error("attempt to access '%s', which isn't inside '%s' (forbidden)", resolved, server->cwd);
        http_client_serve_403(client, hdr, ep);
        return;
    }
    struct stat st;
    int ret = stat(full_rel_path, &st);
    if (ret < 0) {
        log_error("couldn't stat '%s'", full_rel_path);
        http_client_serve_404(client, hdr, ep);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        // serve directory
        http_char_buffer_t buf = build_directory_buffer(full_rel_path, ep);
        if (is_error(*ep)) {
            print_error(*ep);
            http_client_serve_500(client, hdr, ep);
            free(buf.data);
            return;
        }
        size_t final_buffer_size = 512 + buf.len;
        char* final_buffer = safe_malloc(final_buffer_size, ep);
        if (is_error(*ep)) {
            free(buf.data);
            return;
        }
        memset(final_buffer, 0, final_buffer_size);
        sprintf(final_buffer, "<!DOCTYPE html><html>"
                              "<head><title>"
                              "Listing of '/%s'"
                              "</title></head>"
                              "<body>"
                              "<h1>Listing of '/%s'</h1>"
                              "<ul>"
                              "%s"
                              "</ul>" HTTP_SERVER_CREDIT "</body>"
                              "</html>",
            target, target,
            buf.data);
        free(buf.data);
        buf.data = NULL;
        buf.len = 0;
        http_header_data this_hdr = *hdr;
        this_hdr.content_type = "text/html";
        http_client_serve(client, final_buffer, final_buffer_size, &this_hdr, ep);
        free(final_buffer);
        return;
    } else {
        FILE* file = fopen(full_rel_path, "r");
        if (!file) {
            log_error("couldn't open '%s'", full_rel_path);
            perror("fopen");
            http_client_serve_404(client, hdr, ep);
            return;
        }
        char* malloced_buf = (char*)safe_malloc(st.st_size, ep);
        if (is_error(*ep)) {
            fclose(file);
            return;
        }
        size_t n = fread(malloced_buf, 1, st.st_size, file);
        fclose(file);
        http_header_data this_hdr = *hdr;
        const char* ext = get_path_extension(full_rel_path);
        if (strcmp(ext, "html") == 0) {
            this_hdr.content_type = "text/html";
        } else if (strcmp(ext, "css") == 0) {
            this_hdr.content_type = "text/css";
        } else if (strcmp(ext, "js") == 0) {
            this_hdr.content_type = "text/js";
        }
        http_client_serve(client, malloced_buf, n, &this_hdr, ep);
        free(malloced_buf);
        if (n != (size_t)st.st_size) {
            log_warning("read %llu, expected to read %llu", (unsigned long long)n, (unsigned long long)st.st_size);
        }
    }
}

const char http_server_rootpage[] = "<!DOCTYPE html>"
                                    "<html>"
                                    "<head>"
                                    "<title>http-server 1.0</title>"
                                    "</head>"
                                    "<body>"
                                    "<h1>http-server 1.0</h1>"
                                    "<p>"
                                    "This page is being served by an instance of "
                                    "<a href=\"https://github.com/lionkor/http\"><code>lionkor/http</code></a>."
                                    "</p>" HTTP_SERVER_CREDIT
                                    "</body>"
                                    "</html>";
const size_t http_server_rootpage_size = sizeof(http_server_rootpage) - 1;
const char http_server_err_404_page[] = "<!DOCTYPE html>"
                                        "<html>"
                                        "<head>"
                                        "<title>404 Not Found</title>"
                                        "</head>"
                                        "<body>"
                                        "<h1>404 Not Found</h1>"
                                        "<p>"
                                        "The requested resource was not found."
                                        "</p>" HTTP_SERVER_CREDIT
                                        "</body>"
                                        "</html>";
const size_t http_server_err_404_page_size = sizeof(http_server_err_404_page) - 1;
const char http_server_err_403_page[] = "<!DOCTYPE html>"
                                        "<html>"
                                        "<head>"
                                        "<title>403 Forbidden</title>"
                                        "</head>"
                                        "<body>"
                                        "<h1>403 Forbidden</h1>"
                                        "<p>"
                                        "Access to this resource is forbidden."
                                        "</p>" HTTP_SERVER_CREDIT
                                        "</body>"
                                        "</html>";
const size_t http_server_err_403_page_size = sizeof(http_server_err_403_page) - 1;
const char http_server_err_500_page[] = "<!DOCTYPE html>"
                                        "<html>"
                                        "<head>"
                                        "<title>500 Internal Server Error</title>"
                                        "</head>"
                                        "<body>"
                                        "<h1>500 Internal Server Error</h1>"
                                        "<p>"
                                        "The server ran into an internal error trying to serve this request."
                                        "</p>" HTTP_SERVER_CREDIT
                                        "</body>"
                                        "</html>";
const size_t http_server_err_500_page_size = sizeof(http_server_err_500_page) - 1;

void* http_thread_pool_main(void* args_ptr) {
    http_thread_pool_main_args* args = args_ptr;
    http_thread_pool* pool = args->pool;
    size_t index = args->index;
    struct timespec wait;
    http_sleep_ms(index * 250);
    while (!atomic_load(&pool->shutdown)) {
        clock_gettime(CLOCK_REALTIME, &wait);
        if (wait.tv_nsec > 500) {
            wait.tv_sec += 1;
            wait.tv_nsec = 0;
        } else {
            wait.tv_nsec += HTTP_MS_TO_NS(500);
        }
        http_thread_pool_fn_t fn = NULL;
        void* arg = NULL;
        pthread_mutex_lock(&pool->jobs_mutexes[index]);
        pthread_cond_timedwait(&pool->condition_vars[index], &pool->jobs_mutexes[index], &wait);
        if (pool->jobs[index]) {
            fn = pool->jobs[index];
            arg = pool->args[index];
            pool->args[index] = NULL;
        }
        pthread_mutex_unlock(&pool->jobs_mutexes[index]);
        if (fn) {
            fn(arg);
            pthread_mutex_lock(&pool->jobs_mutexes[index]);
            pool->jobs[index] = NULL;
            pthread_mutex_unlock(&pool->jobs_mutexes[index]);
        } else {
            sched_yield();
        }
    }
    free(args);
    return NULL;
}

http_thread_pool* http_thread_pool_new(http_error_t* ep) {
    *ep = new_error_ok();
    http_thread_pool* pool = safe_malloc(sizeof(http_thread_pool), ep);
    if (!is_error(*ep)) {
        log_info("building thread pool of %d threads", HTTP_THREAD_POOL_SIZE);
        memset(pool, 0, sizeof(http_thread_pool));
        atomic_store(&pool->shutdown, false);
        for (size_t i = 0; i < HTTP_THREAD_POOL_SIZE; ++i) {
            int res;
            res = pthread_condattr_init(&pool->condition_vars_attrs[i]);
            if (res != 0) {
                perror("pthread_condattr_init");
                *ep = new_error_error("failed to init condattr");
                return NULL;
            }
            res = pthread_cond_init(&pool->condition_vars[i], &pool->condition_vars_attrs[i]);
            if (res != 0) {
                perror("pthread_cond_init");
                *ep = new_error_error("failed to init cond");
                return NULL;
            }
            res = pthread_mutexattr_init(&pool->jobs_mutexes_attrs[i]);
            if (res != 0) {
                perror("pthread_mutexattr_init");
                *ep = new_error_error("failed to init mutexattr");
                return NULL;
            }
            res = pthread_mutex_init(&pool->jobs_mutexes[i], &pool->jobs_mutexes_attrs[i]);
            if (res != 0) {
                perror("pthread_mutex_init");
                *ep = new_error_error("failed to init mutex");
                return NULL;
            }
            res = pthread_attr_init(&pool->attrs[i]);
            if (res != 0) {
                perror("pthread_attr_init");
                *ep = new_error_error("failed to init thread attr");
                return NULL;
            }
            http_thread_pool_main_args* args = safe_malloc(sizeof(http_thread_pool_main_args), ep);
            if (is_error(*ep)) {
                return NULL;
            }
            args->index = i;
            args->pool = pool;
            res = pthread_create(&pool->threads[i], &pool->attrs[i], http_thread_pool_main, args);
            if (res != 0) {
                perror("pthread_create");
                *ep = new_error_error("failed to create thread");
                return NULL;
            }
        }
    }
    return pool;
}

void http_thread_pool_destroy(http_thread_pool* pool) {
    if (pool) {
        for (size_t i = 0; i < HTTP_THREAD_POOL_SIZE; ++i) {
            int detachstate = 0;
            pthread_attr_getdetachstate(&pool->attrs[i], &detachstate);
            if (detachstate == PTHREAD_CREATE_JOINABLE) {
                log_info("joining thread %lu", pool->threads[i]);
                pthread_join(pool->threads[i], NULL);
            }
            pthread_mutexattr_destroy(&pool->jobs_mutexes_attrs[i]);
            pthread_mutex_destroy(&pool->jobs_mutexes[i]);
            pthread_cond_destroy(&pool->condition_vars[i]);
            pthread_condattr_destroy(&pool->condition_vars_attrs[i]);
            pthread_attr_destroy(&pool->attrs[i]);
        }
    }
    free(pool);
}

void http_thread_pool_add_job(http_thread_pool* pool, http_thread_pool_fn_t job, void* arg, http_error_t* ep) {
    *ep = new_error_ok();
    bool found = false;
    static size_t last_i = 0;
    size_t i = (last_i) % HTTP_THREAD_POOL_SIZE;
    while (!found) {
        if (!pool->jobs[i]) {
            pthread_mutex_lock(&pool->jobs_mutexes[i]);
            if (!pool->jobs[i]) {
                pool->jobs[i] = job;
                pool->args[i] = arg;
                found = true;
                last_i = 0;
                pthread_mutex_unlock(&pool->jobs_mutexes[i]);
                pthread_cond_signal(&pool->condition_vars[i]);
                break;
            }
            pthread_mutex_unlock(&pool->jobs_mutexes[i]);
        }
        ++i;
        i %= HTTP_THREAD_POOL_SIZE;
    }
    last_i = i;
    if (!found) {
        *ep = new_error_error("failed to find empty job slot");
    }
}

void http_sleep_ms(long ms) {
    struct timespec rem;
    struct timespec req = {
        (int)(ms / 1000), /* secs (Must be Non-Negative) */
        (ms % 1000) * 1000000 /* nano (Must be in range of 0 to 999999999) */
    };

    (void)nanosleep(&req, &rem);
}
