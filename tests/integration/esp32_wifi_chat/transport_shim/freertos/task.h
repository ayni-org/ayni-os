#pragma once
#include "FreeRTOS.h"
#include <atomic>
#include <chrono>
#include <thread>

using TaskHandle_t = std::atomic<uint32_t> *;
inline void xTaskNotifyGive(TaskHandle_t task) {
    ++*task;
}
inline unsigned ulTaskNotifyTake(int, TickType_t) {
    return 0;
}
inline void vTaskDelay(TickType_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
