#pragma once

#include "error_t.h"

#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
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

typedef struct {
    char* data;
    size_t len;
} http_char_buffer_t;

typedef void (*http_client_connect_cb)(http_server*, http_client*);

//TODO most ptr parameters can be const
http_server* http_server_new(http_error_t*);
void http_server_free(http_server*);
void http_server_start(http_server*, uint16_t port, http_error_t*);
void http_server_accept_client(http_server*, http_client_connect_cb, http_error_t*);
void http_client_serve(http_client*, const char* body, size_t body_size, http_header_data*, http_error_t*);
void http_client_set_rcv_timeout(http_client*, time_t seconds, suseconds_t microseconds, http_error_t*);
void http_client_receive_header(http_client*, http_header*, http_error_t*);
void http_header_parse_field(http_header*, char* value_buf, size_t value_buf_size, const char* fieldname, http_error_t*);

// a few helpers for common error pages
void http_client_serve_404(http_client* client, const http_header_data* template_hdr_data, http_error_t*);
void http_client_serve_403(http_client* client, const http_header_data* template_hdr_data, http_error_t*);
void http_client_serve_500(http_client* client, const http_header_data* template_hdr_data, http_error_t*);
void http_client_serve_file(http_client*, http_server*, const char* target, const http_header_data* template_hdr_data, http_error_t*);

#ifndef HTTP_THREAD_POOL_SIZE
#define HTTP_THREAD_POOL_SIZE 8
#endif
typedef void (*http_thread_pool_fn_t)(void*);
typedef struct {
    pthread_t threads[HTTP_THREAD_POOL_SIZE];
    pthread_attr_t attrs[HTTP_THREAD_POOL_SIZE];
    http_thread_pool_fn_t jobs[HTTP_THREAD_POOL_SIZE];
    void* args[HTTP_THREAD_POOL_SIZE];
    pthread_mutex_t jobs_mutexes[HTTP_THREAD_POOL_SIZE];
    pthread_mutexattr_t jobs_mutexes_attrs[HTTP_THREAD_POOL_SIZE];
    pthread_cond_t condition_vars[HTTP_THREAD_POOL_SIZE];
    pthread_condattr_t condition_vars_attrs[HTTP_THREAD_POOL_SIZE];
    atomic_bool shutdown;
} http_thread_pool;

typedef struct {
    http_thread_pool* pool;
    size_t index;
} http_thread_pool_main_args;

#define HTTP_MS_TO_NS(x) ((x)*1000000L)

http_thread_pool* http_thread_pool_new(http_error_t* ep);
void* http_thread_pool_main(void* args_ptr);
void http_thread_pool_destroy(http_thread_pool* pool);
void http_thread_pool_add_job(http_thread_pool* pool, http_thread_pool_fn_t job, void* arg, http_error_t* ep);

// utils

// index or -1 if not found
ssize_t http_search_for_string(const char* in, size_t in_size, const char* what, size_t what_size);
void http_sleep_ms(long ms);

extern const char http_server_rootpage[];
extern const size_t http_server_rootpage_size;
extern const char http_server_err_404_page[];
extern const size_t http_server_err_404_page_size;
extern const char http_server_err_403_page[];
extern const size_t http_server_err_403_page_size;
extern const char http_server_err_500_page[];
extern const size_t http_server_err_500_page_size;

#define HTTP_SERVER_CREDIT "<br><br><hr><small><a href=\"https://github.com/lionkor/http\">lionkor/http</a> v1.0</small>"

#define HTTP_KB 1024
#define HTTP_MB HTTP_KB * 1024
#define HTTP_GB HTTP_MB * 1024
