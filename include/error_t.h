#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* error;
} error_t;

error_t new_error_ok(void);
error_t new_error_error(const char* msg);
bool is_error(error_t);
bool is_ok(error_t);
void print_error(error_t);
