#pragma once
#include "motor_plan.h"
#include <ayni/control.h>
#include <cstring>
namespace usbchat {
inline constexpr uint32_t motor_version = 1, motor_procedure_us = 60000000,
                          motor_sample_age_us = 100000, motor_poll_us = 20000;
inline constexpr uint64_t motor_domain = 1;
inline constexpr const char *motor_fields[]{"motor1_percent", "motor2_percent", "duration_ms"};
inline constexpr const char *motor_scene = "motor.manual-bench";
inline ayni_string_v1 motor_text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
inline ayni_control_envelope_v1 motor_limits() {
    ayni_control_envelope_v1 limits{};
    limits.count = 3;
    for (uint32_t at = 0; at < limits.count; ++at) {
        auto &range = limits.fields[at];
        const auto field = motor_text(motor_fields[at]);
        const auto unit = motor_text(at == 2 ? "ms" : "percent");
        range.field_id.size = field.size;
        std::memcpy(range.field_id.data, field.data, field.size);
        range.unit.size = unit.size;
        std::memcpy(range.unit.data, unit.data, unit.size);
        range.minimum = at == 2 ? int64_t(motor_duration_ms_min) : -int64_t(motor_percent_max);
        range.maximum = at == 2 ? motor_plan_duration_ms_max : motor_percent_max;
    }
    return limits;
}
} // namespace usbchat
