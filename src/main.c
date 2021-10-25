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

#define HTTP_THREAD_POOL_SIZE 8
typedef void (*http_thread_pool_fn_t)(http_server*, http_client*, error_t*);
typedef struct {
    pthread_t threads[HTTP_THREAD_POOL_SIZE];
    pthread_attr_t attrs[HTTP_THREAD_POOL_SIZE];
    http_thread_pool_fn_t jobs[HTTP_THREAD_POOL_SIZE];
    pthread_mutex_t jobs_mutexes[HTTP_THREAD_POOL_SIZE];
    pthread_mutexattr_t jobs_mutexes_attrs[HTTP_THREAD_POOL_SIZE];
    atomic_bool shutdown;
} http_thread_pool;

typedef struct {
    http_thread_pool* pool;
    size_t index;
    http_server* server;
    http_client* client;
} http_thread_pool_main_args;

void* http_thread_pool_main(void* args_ptr) {
    http_thread_pool_main_args* args = args_ptr;
    http_thread_pool* pool = args->pool;
    size_t index = args->index;
    while (!atomic_load(&pool->shutdown)) {
        error_t err = new_error_ok();
        http_thread_pool_fn_t fn = NULL;
        pthread_mutex_lock(&pool->jobs_mutexes[index]);
        if (pool->jobs[index]) {
            fn = pool->jobs[index];
            pool->jobs[index] = NULL;
        }
        pthread_mutex_unlock(&pool->jobs_mutexes[index]);
        if (fn) {
            log_info("found a job in thread %zu", index);
            fn(args->server, args->client, &err);
            if (is_error(err)) {
                log_error("thread %zu, fn %p: %s", index, (void*)fn, err.error);
            }
            log_info("done executing job in thread %zu", index);
        } else {
            sched_yield();
        }
    }
    free(args);
    return NULL;
}

http_thread_pool* http_thread_pool_new(error_t* ep) {
    http_thread_pool* pool = safe_malloc(sizeof(http_thread_pool), ep);
    if (!is_error(*ep)) {
        log_info("building thread pool of %d threads", HTTP_THREAD_POOL_SIZE);
        memset(pool, 0, sizeof(http_thread_pool));
        atomic_store(&pool->shutdown, false);
        for (size_t i = 0; i < HTTP_THREAD_POOL_SIZE; ++i) {
            pthread_attr_init(&pool->attrs[i]);
            pthread_create(&pool->threads[i], &pool->attrs[i], http_thread_pool_main, pool);
            pthread_mutexattr_init(&pool->jobs_mutexes_attrs[i]);
            pthread_mutex_init(&pool->jobs_mutexes[i], &pool->jobs_mutexes_attrs[i]);
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

void http_thread_pool_add_job(http_thread_pool* pool, http_thread_pool_fn_t job, error_t* ep) {
    *ep = new_error_ok();
    bool found = false;
    for (size_t i = 0; i < HTTP_THREAD_POOL_SIZE; ++i) {
        pthread_mutex_lock(&pool->jobs_mutexes[i]);
        if (!pool->jobs[i]) {
            pool->jobs[i] = job;
            found = true;
            pthread_mutex_unlock(&pool->jobs_mutexes[i]);
            break;
        }
        pthread_mutex_unlock(&pool->jobs_mutexes[i]);
    }
    if (!found) {
        *ep = new_error_error("failed to find empty job slot");
    }
}

typedef struct {
    pthread_t id;
    pthread_attr_t attr;
    _Atomic bool active;
    _Atomic bool initialized;
} http_thread;

#define HTTP_THREAD_COUNT 64
http_thread threads[HTTP_THREAD_COUNT];

typedef struct {
    http_server* server;
    http_client* client;
    http_thread* thread;
} http_request_args;

void* handle_client_request_thread_main(void* args_ptr) {
    http_request_args* args = args_ptr;
    http_client* client = args->client;
    http_server* server = args->server;
    bool keep_alive = false;
    log_info("thread %ld is pid %d", args->thread->id, getpid());

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
    atomic_store(&args->thread->active, false);
    return NULL;
}

void handle_client_request(http_server* server, http_client* client) {
    log_info("%s: %p", "got client", (void*)client);
    http_thread* thread = NULL;
    bool expected = false;
    bool desired = true;
    size_t tries = 0;
    long ms = 1000;
    while (!thread) {
        for (size_t i = 0; i < HTTP_THREAD_COUNT; ++i) {
            if (atomic_compare_exchange_strong(&threads[i].active, &expected, desired)) {
                // we can only ask more info if its initialized
                if (atomic_load(&threads[i].initialized)) {
                    int detachstate = 0;
                    pthread_attr_getdetachstate(&threads[i].attr, &detachstate);
                    if (detachstate == PTHREAD_CREATE_JOINABLE) {
                        log_info("%ld is joinable", threads[i].id);
                        int ret = pthread_join(threads[i].id, NULL);
                        log_info("%ld was joined", threads[i].id);
                        if (ret != 0) {
                            perror("pthread_join");
                            // continue anyways
                        }
                    }
                    pthread_attr_destroy(&threads[i].attr);
                }
                thread = &threads[i];
                break;
            }
        }
        if (thread) {
            break;
        }
        ++tries;
        if (tries > 10) {
            tries = 0;
            log_info("waiting for too long, speeding up to %ldms", ms);
            ms /= 2;
        }
        sleep_ms(ms);
    }
    assert(thread);
    error_t err = new_error_ok();
    http_request_args* args = safe_malloc(sizeof(http_request_args), &err);
    if (is_error(err)) {
        print_error(err);
        log_error("%s", "fatal - could not allocate resources to create a new thread");
        atomic_store(&thread->active, false);
        return;
    }
    args->client = client;
    args->server = server;
    args->thread = thread;
    pthread_attr_init(&args->thread->attr);
    pthread_create(&args->thread->id, &args->thread->attr, handle_client_request_thread_main, (void*)args);
    atomic_store(&args->thread->initialized, true);
    log_info("created thread for client %p with id %ld", (void*)client, thread->id);
}

void handle_signals(int sig) {
    switch (sig) {
    case SIGINT:
        exit(2);
        break;
    }
}

int main(void) {
    memset(threads, 0, sizeof(threads));
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
