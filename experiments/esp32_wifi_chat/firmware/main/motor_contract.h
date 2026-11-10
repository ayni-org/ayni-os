#pragma once
#include "motor_plan.h"
#include <ayni/control.h>
#include <ayni/values.h>
#include <cstring>
namespace wifichat {
inline constexpr uint32_t motor_version = 2, motor_procedure_us = 65000000,
                          motor_sample_age_us = 100000, motor_poll_us = 20000;
inline constexpr uint64_t motor_domain = 1;
// Contract limits of the four motor tools; their inputs hold only integers.
inline constexpr uint32_t motor_input_bytes_max = 256, motor_reply_bytes_max = 1024;
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
// The same closed machine input is registered by Motor and checked by native consumers.
inline std::array<ayni_schema_node_v1, 4> motor_run_schema() {
    std::array<ayni_schema_node_v1, 4> nodes{};
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = nodes.size();
    const auto limits = motor_limits();
    for (uint32_t at = 0; at < limits.count; ++at) {
        auto &node = nodes[at + 1];
        node.kind = AYNI_VALUE_I64;
        node.field_id = at + 1;
        node.subtree_end = at + 2;
        node.flags = AYNI_SCHEMA_REQUIRED;
        node.name = motor_text(motor_fields[at]);
        node.unit = motor_text(at == limits.count - 1 ? "ms" : "percent");
        node.min_i64 = limits.fields[at].minimum;
        node.max_i64 = limits.fields[at].maximum;
    }
    return nodes;
}
} // namespace wifichat
