#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char* error;
} error_t;

inline error_t new_error_ok(void);
inline error_t new_error_error(const char* msg);
inline bool is_error(error_t);
inline bool is_ok(error_t);
inline void print_error(error_t);
