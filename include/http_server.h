#pragma once

#include "error_t.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#define CRLF "\r\n"
#define HTTP_HEADER_SIZE_MAX 4096
typedef int socket_t;

typedef struct {
    socket_t socket;
    int backlog;
    char cwd[128];
    bool show_root_page;
} http_server;

// server-side info about a client
typedef struct {
    struct sockaddr address;
    socklen_t address_len;
    socket_t socket;
    // none if zero
    struct timeval rcv_timeout;
} http_client;

// buffers for header data to be received into
typedef struct {
    char method[8];
    char target[128];
    char version[16];
    char host[64];
    char buffer[HTTP_HEADER_SIZE_MAX];
    size_t start_of_headers;
} http_header;

// used in *_serve functions to provide header data
typedef struct {
    int status_code;
    const char* status_message;
    const char* content_type;
    const char* connection;
    const char* additional_headers;
} http_header_data;

typedef void (*http_client_connect_cb)(http_server*, http_client*);

http_server* new_http_server(error_t*);
void free_http_server(http_server*);
void start_http_server(http_server*, uint16_t port, error_t*);
void http_server_accept_client(http_server*, http_client_connect_cb, error_t*);
void http_client_serve(http_client*, const char* body, size_t body_size, http_header_data*, error_t*);
void http_client_set_rcv_timeout(http_client*, time_t seconds, suseconds_t microseconds, error_t*);
void http_client_receive_header(http_client*, http_header*, error_t*);
void http_header_parse_field(http_header*, char* value_buf, size_t value_buf_size, const char* fieldname, error_t*);

// utils

// index or -1 if not found
ssize_t search_for_string(const char* in, size_t in_size, const char* what, size_t what_size);

extern const char http_server_rootpage[];
extern const size_t http_server_rootpage_size;
extern const char http_server_err_404_page[];
extern const size_t http_server_err_404_page_size;
extern const char http_server_err_403_page[];
extern const size_t http_server_err_403_page_size;
extern const char http_server_err_500_page[];
extern const size_t http_server_err_500_page_size;

#define HTTP_SERVER_CREDIT "<br><br><hr><small><a href=\"https://github.com/lionkor\">lionkor/http</a> v1.0</small>"
