#pragma once
#include <cstddef>
#include <cstdint>
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
    explicit ModelBuffer(size_t bytes) : size(bytes) {
        data = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    ModelBuffer(const ModelBuffer &) = delete;
    ModelBuffer &operator=(const ModelBuffer &) = delete;
    ~ModelBuffer() { reset(); }
    void reset() {
        erase_free(data, size);
        data = nullptr;
        size = 0;
    }
};
} // namespace wifichat::wifi
