#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void *heap_caps_malloc(size_t size, unsigned caps);
void *heap_caps_calloc(size_t count, size_t size, unsigned caps);
void heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(unsigned caps);
size_t heap_caps_get_largest_free_block(unsigned caps);
size_t heap_caps_get_minimum_free_size(unsigned caps);

#define MALLOC_CAP_SPIRAM (1U << 10)
#define MALLOC_CAP_INTERNAL (1U << 11)
#define MALLOC_CAP_DMA (1U << 12)
#define MALLOC_CAP_8BIT (1U << 2)
