#pragma once

#include <stdio.h>

#define ESP_LOGE(tag, fmt, ...)                                                            \
    do {                                                                                   \
        printf("E %s: " fmt "\n", (tag), ##__VA_ARGS__);                                   \
        fflush(stdout);                                                                    \
    } while (0)
#define ESP_LOGW(tag, fmt, ...)                                                            \
    do {                                                                                   \
        printf("W %s: " fmt "\n", (tag), ##__VA_ARGS__);                                   \
        fflush(stdout);                                                                    \
    } while (0)
#define ESP_LOGI(tag, fmt, ...)                                                            \
    do {                                                                                   \
        printf("I %s: " fmt "\n", (tag), ##__VA_ARGS__);                                   \
        fflush(stdout);                                                                    \
    } while (0)
#define ESP_LOGD(tag, fmt, ...)                                                            \
    do {                                                                                   \
        printf("D %s: " fmt "\n", (tag), ##__VA_ARGS__);                                   \
        fflush(stdout);                                                                    \
    } while (0)
