#pragma once
#include <array>
#include <ayni/calls.h>

namespace wifichat {
struct OutputInterval {
    uint64_t call = 0, start_us = 0, stop_us = 0; // Zero stop means no known output end yet.
};
using OutputIntervals = std::array<OutputInterval, AYNI_CALL_TOOL_COUNT_MAX>;
} // namespace wifichat
