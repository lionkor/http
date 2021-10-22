#pragma once

#include "error_t.h"
#include <string.h>

void* safe_malloc(size_t size, error_t*);
