#pragma once
#include <cstdint>
namespace wifichat {
// Policy affects new natural-language instructions only; explicit cancel/stop always stops.
// continue is a C++ keyword, hence the source spelling continue_motion.
enum class BargeInPolicy : uint32_t { stop, hold, continue_motion };
inline constexpr uint32_t default_hold_scale_percent = 25;
} // namespace wifichat
