#pragma once
#include "FreeRTOS.h"
#include <cstddef>

struct TestQueue;
using QueueHandle_t = TestQueue *;
QueueHandle_t xQueueCreate(size_t capacity, size_t item_size);
int xQueueSend(QueueHandle_t, const void *, TickType_t);
int xQueuePeek(QueueHandle_t, void *, TickType_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
