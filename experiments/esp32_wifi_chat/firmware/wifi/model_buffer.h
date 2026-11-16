#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <esp_heap_caps.h>
namespace wifichat::wifi {
inline void erase_free(uint8_t *data, size_t bytes) {
    if (!data) {
        return;
    }
    volatile uint8_t *at = data;
    for (size_t index = 0; index < bytes; ++index) {
        at[index] = 0;
    }
    heap_caps_free(data);
}
// Exclusive PSRAM scratch lane, cleared before release; no fallback into internal/DMA RAM.
struct ModelBuffer {
    uint8_t *data = nullptr;
    size_t size = 0;
    ModelBuffer() = default;
    explicit ModelBuffer(size_t bytes) : size(bytes) {
        data = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!data) {
            size = 0;
        }
    }
    ModelBuffer(const ModelBuffer &) = delete;
    ModelBuffer &operator=(const ModelBuffer &) = delete;
    ~ModelBuffer() { reset(); }
    // Preserve the Writer prefix in bounded PSRAM. Failure keeps the old lane intact;
    // replaced private storage is erased before free, including after a relocation.
    bool grow(size_t minimum, size_t maximum) {
        if (minimum > maximum) {
            return false;
        }
        if (minimum <= size) {
            return true;
        }
        constexpr size_t initial_bytes = 1024, growth = 2;
        const auto doubled = size > maximum / growth ? maximum : size * growth;
        const auto capacity =
            std::min(maximum, std::max(minimum, std::max(initial_bytes, doubled)));
        auto *next =
            static_cast<uint8_t *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!next) {
            return false;
        }
        if (data) {
            std::memcpy(next, data, size);
        }
        erase_free(data, size);
        data = next;
        size = capacity;
        return true;
    }
    void reset() {
        erase_free(data, size);
        data = nullptr;
        size = 0;
    }
};
} // namespace wifichat::wifi
