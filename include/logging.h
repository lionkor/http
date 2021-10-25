#pragma once

#include <stdio.h>
#include <unistd.h>

#define log_info(fmt, ...) printf("%d %.20s info: " fmt "\n", gettid(), __func__, __VA_ARGS__)
#define log_error(fmt, ...) printf("%d %.20s error: " fmt "\n", gettid(), __func__, __VA_ARGS__)
#define log_warning(fmt, ...) printf("%d %.20s warning: " fmt "\n", gettid(), __func__, __VA_ARGS__)
