// Native wall-clock shim for production file-operation accounting.
#ifndef AYNI_TEST_ESP_TIMER_H
#define AYNI_TEST_ESP_TIMER_H
#include <chrono>
#include <cstdint>
inline int64_t esp_timer_get_time() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
#endif
