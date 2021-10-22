#pragma once

#include <stdio.h>

#define log_info(fmt, ...) printf("info: " fmt "\n", __VA_ARGS__)
#define log_error(fmt, ...) printf("error: " fmt "\n", __VA_ARGS__)
#define log_warning(fmt, ...) printf("warning: " fmt "\n", __VA_ARGS__)
