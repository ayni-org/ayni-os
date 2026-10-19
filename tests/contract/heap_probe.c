/* Deliberately allocates, so the heap check can prove it detects allocators. */
#include <stdlib.h>

void *ayni_heap_probe(size_t size);

void *ayni_heap_probe(size_t size) {
    return malloc(size);
}
