#include "memory.h"
#include <stdlib.h>

void* safe_malloc(size_t size, error_t* ep) {
    void* ptr = malloc(size);
    if (!ptr) {
        *ep = new_error_error("ouf of memory");
        return NULL;
    }
    return ptr;
}
