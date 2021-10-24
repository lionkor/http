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
    hdr.additional_headers = "Keep-Alive: timeout=10" CRLF
                             "Server: lionkor/http" CRLF;
    hdr.connection = "close";
    hdr.status_code = 200;
    hdr.status_message = "OK";

    do {
        err = new_error_ok();
        log_info("%s", "receiving and parsing header");
        http_header header;

        http_client_receive_header(client, &header, &err);
        log_info("errno is %d", errno);
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
                log_info("%s", "keep alive");
                keep_alive = true;
                hdr.connection = "keep-alive";
            } else {
                log_info("%s", "don't keep alive");
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
    http_server* server = http_server_new(&err);
    if (is_error(err)) {
        print_error(err);
        return __LINE__;
    }
    server->backlog = 10;
    server->show_root_page = false;
    http_server_start(server, 11000, &err);
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
    http_server_free(server);
    log_info("%s", "http-server terminated");
}
