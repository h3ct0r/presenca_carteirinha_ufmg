#pragma once

// Native stand-in for the ESP heap-caps allocator: plain malloc family.

#include <stdlib.h>

#define MALLOC_CAP_DEFAULT 0u
#define MALLOC_CAP_SPIRAM 0u
#define MALLOC_CAP_INTERNAL 0u
#define MALLOC_CAP_8BIT 0u
#define MALLOC_CAP_DMA 0u

static inline void* heap_caps_malloc(size_t size, unsigned int) { return malloc(size); }
static inline void* heap_caps_calloc(size_t n, size_t size, unsigned int) {
    return calloc(n, size);
}
static inline void* heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned int) {
    void* p = nullptr;
    if (posix_memalign(&p, alignment, size) != 0) return nullptr;
    return p;
}
