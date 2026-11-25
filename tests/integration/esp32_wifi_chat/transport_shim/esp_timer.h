#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
// -1 selects native monotonic time. A test owner may bind ESP time to its Sim clock.
inline std::atomic<int64_t> esp_test_time_us{-1};
inline int64_t esp_timer_get_time() {
    const auto fixed = esp_test_time_us.load();
    return fixed >= 0 ? fixed
                      : std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
}
