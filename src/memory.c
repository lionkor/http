#include "memory.h"
#include <stdlib.h>

void* safe_malloc(size_t size, http_error_t* ep) {
    void* ptr = malloc(size);
    if (!ptr) {
        *ep = http_new_error_error("out of memory");
        return NULL;
    }
    return ptr;
}
