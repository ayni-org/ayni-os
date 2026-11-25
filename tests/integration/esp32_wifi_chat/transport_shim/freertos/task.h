#pragma once
#include "FreeRTOS.h"
#include <atomic>
#include <chrono>
#include <thread>

struct StaticTask_t {};
using StackType_t = uint8_t;
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
