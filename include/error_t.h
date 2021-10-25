#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* error;
} http_error_t;

http_error_t new_error_ok(void);
http_error_t new_error_error(const char* msg);
bool is_error(http_error_t);
bool is_ok(http_error_t);
void print_error(http_error_t);
