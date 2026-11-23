#pragma once
#include <cstdint>
#include <mutex>

using TickType_t = uint32_t;
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED                                                               \
    {}
constexpr int pdTRUE = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr unsigned portTICK_PERIOD_MS = 1;
