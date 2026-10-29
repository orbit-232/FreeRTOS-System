#pragma once
#include <stddef.h>
void *heap_caps_malloc(size_t, unsigned);
void *heap_caps_calloc(size_t, size_t, unsigned);
void heap_caps_free(void *);
#define MALLOC_CAP_SPIRAM (1u<<10)
#define MALLOC_CAP_8BIT (1u<<2)
