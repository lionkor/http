#pragma once

#include "error_t.h"
#include <string.h>

void* safe_malloc(size_t size, http_error_t*);
