#pragma once

#include "error_t.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

typedef int socket_t;

typedef struct {
    socket_t socket;
    int backlog;
} http_server;

// server-side info about a client
typedef struct {
    struct sockaddr address;
    socklen_t address_len;
    socket_t socket;
} http_client;

typedef void (*http_client_connect_cb)(http_client*);

http_server* new_http_server(error_t*);
void free_http_server(http_server*);
// blocking
void start_http_server(http_server*, uint16_t port, error_t*);
void http_server_accept_client(http_server*, http_client_connect_cb, error_t*);

typedef struct {
    char method[8];
    char target[128];
    char version[16];
    char host[64];
} http_header;

void http_client_read_header(http_client*, http_header*, error_t*);
