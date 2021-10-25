#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* error;
} http_error_t;

#define http_new_error_ok() \
    (http_error_t) { NULL }
#define http_new_error_error(msg) \
    (http_error_t) { msg }
#define http_is_error(err) ((err).error != NULL)
#define http_is_ok(err) ((err).error == NULL)
#define http_print_error(err)             \
    do {                                  \
        if ((err).error == NULL) {        \
            log_error("%s", "no error");  \
        } else {                          \
            log_error("%s", (err).error); \
        }                                 \
    } while (0)
