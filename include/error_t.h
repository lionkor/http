#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* error;
} http_error_t;

#define new_error_ok() \
    (http_error_t) { NULL }
#define new_error_error(msg) \
    (http_error_t) { msg }
#define is_error(err) (e.error != NULL)
#define is_ok(err) (e.error == NULL)
#define print_error(err)                 \
    do {                                 \
        if (e.error == NULL) {           \
            log_error("%s", "no error"); \
        } else {                         \
            log_error("%s", e.error);    \
        }                                \
        while (0)
