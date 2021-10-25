#include "error_t.h"
#include "logging.h"

http_error_t new_error_ok() {
    return (http_error_t) { NULL };
}

http_error_t new_error_error(const char* msg) {
    return (http_error_t) { msg };
}

bool is_error(http_error_t e) {
    return e.error != NULL;
}

bool is_ok(http_error_t e) {
    return e.error == NULL;
}

void print_error(http_error_t e) {
    if (e.error == NULL) {
        log_error("%s", "no error");
    } else {
        log_error("%s", e.error);
    }
}
