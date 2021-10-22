#include "error_t.h"
#include "logging.h"

error_t new_error_ok() {
    return (error_t) { NULL };
}

error_t new_error_error(const char* msg) {
    return (error_t) { msg };
}

bool is_error(error_t e) {
    return e.error != NULL;
}

bool is_ok(error_t e) {
    return e.error == NULL;
}

void print_error(error_t e) {
    if (e.error == NULL) {
        log_error("%s", "no error");
    } else {
        log_error("%s", e.error);
    }
}
