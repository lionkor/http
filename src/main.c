#include "http_server.h"
#include "logging.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

size_t requests_handled = 0;

void handle_client_request(http_server*, http_client* client) {
    log_info("%s: %p", "got client", (void*)client);

    bool keep_alive = false;

    do {
        log_info("%s", "receiving and parsing header");
        error_t err = new_error_ok();
        http_header header;

        http_client_receive_header(client, &header, &err);
        if (is_error(err)) {
            print_error(err);
            log_error("%s", "request failed");
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
                http_server_serve(client, buf, sizeof(buf) - 1, "text/plain", &err);
                if (is_error(err)) {
                    print_error(err);
                }
            } else {
                http_server_serve_error_page(client, 404, &err);
            }
        }
        ++requests_handled;
        if (requests_handled % 1000 == 0) {
            fprintf(stderr, "requests handled: %llu\n", (unsigned long long)requests_handled);
        }
    } while (keep_alive);

    shutdown(client->socket, SHUT_RD);
    close(client->socket);
    free(client);
}

int main(void) {
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
