#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace wifichat {
inline constexpr int32_t motor_percent_scale = 100;
inline constexpr int32_t motor_percent_max = 90;
inline constexpr uint32_t motor_plan_duration_ms_max = 60000;
inline constexpr uint32_t motor_duration_ms_min = 50, motor_duration_step_ms = 5;
// User's 2026-11-01 wiring revision, in driver input order D0, D1, D2, D3.
inline constexpr std::array<uint32_t, 4> motor_pins{38, 39, 40, 41};
// User's 2026-11-13 board calibration: only motor 2 reverses the signed input direction.
inline constexpr std::array<bool, 2> motor_reversed{false, true};
inline constexpr size_t motor_inputs_per_channel = 2;
// Canonical raw-PWM request, not measured wheel speed or position.
struct MotorCommand {
    int32_t first_percent = 0, second_percent = 0;
    uint32_t duration_ms = 0;
};
inline bool valid_motor_command(const MotorCommand &command) {
    return command.first_percent >= -motor_percent_max &&
           command.first_percent <= motor_percent_max &&
           command.second_percent >= -motor_percent_max &&
           command.second_percent <= motor_percent_max &&
           command.duration_ms >= motor_duration_ms_min &&
           command.duration_ms <= motor_plan_duration_ms_max &&
           command.duration_ms % motor_duration_step_ms == 0;
}
// Return the D0..D3 index for a valid motor and Schema-normalized signed percentage.
// Planning, RMT selection and PCNT expectations share the board's one polarity rule.
inline size_t motor_input(size_t motor, int32_t percent) {
    const size_t direction = (percent < 0) != motor_reversed[motor] ? 1 : 0;
    return motor * motor_inputs_per_channel + direction;
}
// Schema-normalized magnitude shared by software planning and the hardware waveform.
inline uint32_t motor_duty_percent(int32_t percent) {
    return uint32_t(percent < 0 ? -percent : percent);
}
// A software plan, not an output sample or permit. Positive selects D0/D3 after
// board calibration; actual wheel motion still needs observation.
// TA6586 datasheet p1: both low coasts; both high brakes (never generated here).
inline std::array<uint32_t, 4> motor_duties(int32_t first, int32_t second) {
    std::array<uint32_t, 4> duties{};
    const std::array<int32_t, motor_reversed.size()> percentages{first, second};
    for (size_t motor = 0; motor < percentages.size(); ++motor) {
        const auto percent = percentages[motor];
        duties[motor_input(motor, percent)] = motor_duty_percent(percent);
    }
    return duties;
}
} // namespace wifichat
