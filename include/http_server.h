#pragma once

#include "error_t.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef int socket_t;

typedef struct {
    socket_t socket;
    int backlog;
    char cwd[128];
} http_server;

// server-side info about a client
typedef struct {
    struct sockaddr address;
    socklen_t address_len;
    socket_t socket;
} http_client;

typedef void (*http_client_connect_cb)(http_server*, http_client*);

http_server* new_http_server(error_t*);
void free_http_server(http_server*);
void start_http_server(http_server*, uint16_t port, error_t*);
void http_server_accept_client(http_server*, http_client_connect_cb, error_t*);
void http_server_serve(http_client*, const char* body, size_t body_size, const char* mime_type, error_t*);
void http_server_serve_error_page(http_client*, size_t error_code, error_t*);

#define HTTP_HEADER_SIZE_MAX 4096
typedef struct {
    char method[8];
    char target[128];
    char version[16];
    char host[64];
    char buffer[HTTP_HEADER_SIZE_MAX];
    size_t start_of_headers;
} http_header;
void http_client_receive_header(http_client*, http_header*, error_t*);

void http_header_parse_field(http_header*, char* value_buf, size_t value_buf_size, const char* fieldname, error_t*);

// utils

// index or -1 if not found
ssize_t search_for_string(const char* in, size_t in_size, const char* what, size_t what_size);
