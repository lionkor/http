#include "http_server.h"
#include "logging.h"
#include "memory.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

size_t requests_handled = 0;

typedef struct {
    http_server* server;
    http_client* client;
} handle_request_arg;

void handle_client_request_thread(void* arg_ptr) {
    handle_request_arg* arg = arg_ptr;
    http_client* client = arg->client;
    http_server* server = arg->server;
    bool keep_alive = false;
    http_error_t err = new_error_ok();

    http_client_set_rcv_timeout(client, 5, 0, &err);
    if (is_error(err)) {
        print_error(err);
        log_error("%s", "socket will not timeout on rcv, high risk of locking up, aborting connection");
        goto shutdown_and_free;
    }

    http_header_data hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.additional_headers = "Keep-Alive: timeout=10" CRLF
                             "Server: lionkor/http" CRLF;
    hdr.connection = "close";
    hdr.status_code = 200;
    hdr.status_message = "OK";

    do {
        err = new_error_ok();
        //log_info("%s", "receiving and parsing header");
        http_header header;

        http_client_receive_header(client, &header, &err);
        //log_info("errno is %d", errno);
        if (is_error(err)) {
            if (errno == 11) {
                log_info("client %p timed out", (void*)client);
            } else {
                log_error("%s", "request failed");
            }
            print_error(err);
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
                //log_info("%s", "keep alive");
                keep_alive = true;
                hdr.connection = "keep-alive";
            } else {
                //log_info("%s", "don't keep alive");
                hdr.connection = "close";
            }
        }

        log_info("serving %s %s", header.method, header.target);

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
                http_client_serve_file(client, server, header.target + 1, &hdr, &err);
                if (is_error(err)) {
                    print_error(err);
                }
            } else {
                http_client_serve_404(client, &hdr, &err);
                if (is_error(err)) {
                    print_error(err);
                }
            }
        }
        log_info("served %s %s", header.method, header.target);
        ++requests_handled;
        if (requests_handled % 1000 == 0) {
            fprintf(stderr, "requests handled: %llu\n", (unsigned long long)requests_handled);
        }
    } while (keep_alive);

shutdown_and_free:
    shutdown(client->socket, SHUT_RD);
    close(client->socket);
    free(client);
    free(arg_ptr);
}

http_thread_pool* pool = NULL;

void handle_client_request(http_server* server, http_client* client) {
    http_error_t err = new_error_ok();
    handle_request_arg* arg = safe_malloc(sizeof(handle_request_arg), &err);
    if (is_error(err)) {
        print_error(err);
        return;
    }
    arg->client = client;
    arg->server = server;
    http_thread_pool_add_job(pool, handle_client_request_thread, (void*)arg, &err);
    assert(is_ok(err));
}

void handle_signals(int sig) {
    switch (sig) {
    case SIGINT:
        atomic_store(&pool->shutdown, true);
        break;
    }
}

const char s_usage[] = "<port>";

int main(int argc, char** argv) {
    signal(SIGINT, handle_signals);
    if (argc != 2) {
        log_error("%s: invalid arguments", argv[0]);
        log_info("Usage:\n%s %s", argv[0], s_usage);
        return __LINE__;
    }
    // parse port
    unsigned int port = 0;
    int n = sscanf(argv[1], "%u", &port);
    if (n != 1) {
        log_error("%s", "failed to parse <port> as number");
        return __LINE__;
    }
    if (port > UINT16_MAX) {
        log_error("port %u outside allowed range (%u-%u)", port, 0u, UINT16_MAX);
        return __LINE__;
    }
    log_info("%s", "welcome to http-server 1.0");
    http_error_t err = new_error_ok();
    http_server* server = http_server_new(&err);
    if (is_error(err)) {
        print_error(err);
        return __LINE__;
    }
    pool = http_thread_pool_new(&err);
    if (is_error(err)) {
        print_error(err);
        return __LINE__;
    }
    server->backlog = 10;
    server->show_root_page = false;
    http_server_start(server, port, &err);
    if (is_error(err)) {
        print_error(err);
        return __LINE__;
    }
    while (!atomic_load(&pool->shutdown)) {
        http_server_accept_client(server, handle_client_request, &err);
        if (is_error(err)) {
            print_error(err);
        }
    }
    http_server_free(server);
    http_thread_pool_destroy(pool);
    log_info("%s", "http-server terminated");
}
