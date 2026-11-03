// The owner loop's wake: the task notification that the ESP Host's wake gives and its
// wait_for_wake takes (index 0 of the owner task). Core gives it for its own completions; the
// I/O task and the speaker worker give it when the owner has device work Core cannot see, so
// the owner sleeps until Core's next deadline instead of polling every tick.
#pragma once
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace usbchat {
inline std::atomic<TaskHandle_t> owner_task{nullptr}; // Set once by the owner task itself.
inline void wake_owner() {
    if (auto *task = owner_task.load(std::memory_order_acquire)) {
        xTaskNotifyGive(task);
    }
}
// Owner task only: the Host's wait_for_wake before the Host exists, rounded up to ticks. Wakes
// given while the owner was busy are kept and end the next wait at once.
inline bool wait_for_wake(uint64_t timeout_us) {
    constexpr uint64_t tick_us = uint64_t(portTICK_PERIOD_MS) * 1000;
    const uint64_t ticks = timeout_us / tick_us + (timeout_us % tick_us != 0);
    return ulTaskNotifyTake(pdTRUE,
                            ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : TickType_t(ticks)) != 0;
}
} // namespace usbchat
