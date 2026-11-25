#pragma once
#include <cstddef>
#include <cstdlib>
constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_SPIRAM = 4;
inline void *heap_caps_malloc(size_t size, unsigned) {
    return std::malloc(size);
}
inline void heap_caps_free(void *p) {
    std::free(p);
}
inline void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned) {
    void *out = nullptr;
    return posix_memalign(&out, alignment, size) == 0 ? out : nullptr;
}
inline size_t heap_caps_get_free_size(unsigned) {
    return 1024 * 1024;
}
inline size_t heap_caps_get_largest_free_block(unsigned) {
    return 1024 * 1024;
}
inline size_t heap_caps_get_minimum_free_size(unsigned) {
    return 1024 * 1024;
}
