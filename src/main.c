#include "http_server.h"
#include "logging.h"

#include <stdlib.h>

void handle_client_request(http_client* client) {
    log_info("%s: %p", "got client", (void*)client);
    
    error_t err = new_error_ok();
    http_header header;
    
    http_client_read_header(client, &header, &err);
    
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
