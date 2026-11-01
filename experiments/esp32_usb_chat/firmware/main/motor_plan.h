#pragma once
#include <array>
#include <cstdint>
namespace usbchat {
inline constexpr int32_t motor_percent_max = 100;
inline constexpr uint32_t motor_plan_duration_ms_max = 5000;
// User's 2026-11-01 wiring revision, in driver input order D0, D1, D2, D3.
inline constexpr std::array<uint32_t, 4> motor_pins{38, 39, 40, 41};
// A software plan, not an output sample or permit. Inputs are Schema-normalized
// signed percentages. Positive selects D0/D2; wiring determines shaft direction.
// TA6586 datasheet p1: both low coasts; both high brakes (never generated here).
inline std::array<uint32_t, 4> motor_duties(int32_t first, int32_t second) {
    return {first > 0 ? uint32_t(first) : 0, first < 0 ? uint32_t(-first) : 0,
            second > 0 ? uint32_t(second) : 0, second < 0 ? uint32_t(-second) : 0};
}
} // namespace usbchat
