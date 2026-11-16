// Desktop PSRAM boundary spy for ModelBuffer's ordinary capacity/erase contract.
#pragma once
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
namespace heap_spy {
inline std::map<void *, size_t> allocations;
inline bool erased = true, psram_only = true;
} // namespace heap_spy
inline void *heap_caps_malloc(size_t size, unsigned flags) {
    heap_spy::psram_only &= flags == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *memory = std::malloc(size);
    if (memory) {
        std::memset(memory, 0xcc, size);
        heap_spy::allocations[memory] = size;
    }
    return memory;
}
inline void heap_caps_free(void *memory) {
    const auto found = heap_spy::allocations.find(memory);
    if (found != heap_spy::allocations.end()) {
        const auto *bytes = static_cast<const unsigned char *>(memory);
        for (size_t at = 0; at < found->second; ++at) {
            heap_spy::erased &= bytes[at] == 0;
        }
        heap_spy::allocations.erase(found);
    }
    std::free(memory);
}
