#include "http_server.h"

#include "logging.h"
#include "memory.h"

#include <assert.h>
#include <netdb.h>
#include <stdlib.h>
#include <unistd.h>

http_server* new_http_server(error_t* ep) {
    *ep = new_error_ok();
    http_server* server = (http_server*)safe_malloc(sizeof(http_server), ep);
    if (is_error(*ep)) {
        print_error(*ep);
    }
    server->socket = 0;
    server->backlog = 1;
    return server;
}

void free_http_server(http_server* server) {
    free(server);
}

void start_http_server(http_server* server, uint16_t port, error_t* ep) {
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

void http_server_accept_client(http_server* server, http_client_connect_cb on_connect, error_t* ep) {
    assert(server);
    *ep = new_error_ok();
    http_client* client = (http_client*)safe_malloc(sizeof(http_client), ep);
    if (is_error(*ep)) {
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
    on_connect(client);
}

static size_t min_size_t(size_t a, size_t b) {
    return a < b ? a : b;
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

// 4K header space
#define HTTP_HEADER_SIZE_MAX 4096
static _Thread_local char http_header_buffer[HTTP_HEADER_SIZE_MAX];

void http_client_read_header(http_client* client, http_header* header, error_t* ep) {
    assert(client);
    assert(header);
    *ep = new_error_ok();

    memset(header, 0, sizeof(*header));
    memset(http_header_buffer, 0, HTTP_HEADER_SIZE_MAX);

    ssize_t n = read(client->socket, http_header_buffer, HTTP_HEADER_SIZE_MAX);
    if (n < 0) {
        perror("read");
        *ep = new_error_error("read() failed");
        return;
    }
    http_header_buffer[n] = '\0';
    log_info("header: \nHEADER_START\n%s\nHEADER_END", http_header_buffer);
    char* ptr = http_header_buffer;

    // METHOD
    int index = find_next_in_buffer(ptr, n, ' ');
    if (index < 0) {
        *ep = new_error_error("failed to parse http header, can't find METHOD");
        return;
    }
    memcpy(header->method, ptr, index);
    header->method[index] = 0;
    ptr += index + 1;
    log_info("parsed method: '%s'", header->method);

    // TARGET
    index = find_next_in_buffer(ptr, n, ' ');
    if (index < 0) {
        *ep = new_error_error("failed to parse http header, can't find TARGET");
        return;
    }
    memcpy(header->target, ptr, index);
    header->target[index] = 0;
    ptr += index + 1;
    log_info("parsed target: '%s'", header->target);

    // VERSIOn
    index = find_next_crlf_in_buffer(ptr, n);
    if (index < 0) {
        *ep = new_error_error("failed to parse http header, can't find VERSION");
        return;
    }
    memcpy(header->version, ptr, index);
    header->version[index] = 0;
    ptr += index + 2; // crlf
    log_info("parsed version: '%s'", header->version);

    //parse_method(header->method, sizeof(header->method), buf, n);
}
