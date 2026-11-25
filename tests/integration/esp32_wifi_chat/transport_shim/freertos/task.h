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
inline TaskHandle_t xTaskCreateStatic(void (*)(void *), const char *, uint32_t, void *, uint32_t,
                                      StackType_t *, StaticTask_t *) {
    static std::atomic<uint32_t> unused_task{0};
    return &unused_task; // Tests explicitly step the worker; no host background execution.
}
inline void vTaskDelete(TaskHandle_t) {}
inline uint32_t uxTaskGetStackHighWaterMark(TaskHandle_t) {
    return 4096;
}
inline void vTaskSuspend(TaskHandle_t) {}
constexpr int eSuspended = 1;
inline int eTaskGetState(TaskHandle_t) {
    return eSuspended;
}
