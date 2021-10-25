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

void sleep_ms(long ms) {
    struct timespec rem;
    struct timespec req = {
        (int)(ms / 1000), /* secs (Must be Non-Negative) */
        (ms % 1000) * 1000000 /* nano (Must be in range of 0 to 999999999) */
    };

    (void)nanosleep(&req, &rem);
}

size_t requests_handled = 0;
#ifndef HTTP_THREAD_POOL_SIZE
#define HTTP_THREAD_POOL_SIZE 32
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

void* http_thread_pool_main(void* args_ptr) {
    http_thread_pool_main_args* args = args_ptr;
    http_thread_pool* pool = args->pool;
    size_t index = args->index;
    struct timespec wait;
    sleep_ms(index * 250);
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
            // TODO join, destroy mutexes, etc.
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
        exit(2);
        break;
    }
}

int main(void) {
    signal(SIGINT, handle_signals);
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
