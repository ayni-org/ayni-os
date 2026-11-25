#pragma once
#include "FreeRTOS.h"
#include <cstddef>

struct TestQueue;
struct StaticQueue_t {
    unsigned opaque;
};
using QueueHandle_t = TestQueue *;
QueueHandle_t xQueueCreate(size_t capacity, size_t item_size);
int xQueueSend(QueueHandle_t, const void *, TickType_t);
int xQueuePeek(QueueHandle_t, void *, TickType_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
inline QueueHandle_t xQueueCreateStatic(size_t count, size_t bytes, uint8_t *, StaticQueue_t *) {
    return xQueueCreate(count, bytes);
}
inline void vQueueDelete(QueueHandle_t) {
} // TestQueue storage is owned by the test support registry.
