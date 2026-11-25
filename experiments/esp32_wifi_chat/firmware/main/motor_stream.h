// Business semantics of the ESP motor consumer; generic session ownership lives in runtime.
#pragma once
#include "motor_contract.h"
#include <ayni/values.h>
namespace wifichat {
inline constexpr uint32_t stream_field_count = 7, stream_nodes = stream_field_count + 1;
inline constexpr uint32_t stream_m1_min = 1, stream_m1_max = 2, stream_m2_min = 3,
                          stream_m2_max = 4, stream_initial_m1 = 5, stream_initial_m2 = 6,
                          stream_duration = 7;
inline constexpr const char *stream_execute = "motor.stream.execute";
inline constexpr const char *stream_goal_type = "wifi.control.session";
struct MotorStreamInput {
    std::array<int64_t, stream_field_count> fields{}; // Canonical field ID minus one.
    MotorCommand initial{};
    bool contains(int32_t first, int32_t second) const {
        return first >= fields[stream_m1_min - 1] && first <= fields[stream_m1_max - 1] &&
               second >= fields[stream_m2_min - 1] && second <= fields[stream_m2_max - 1];
    }
};
inline std::array<ayni_schema_node_v1, stream_nodes> motor_stream_schema() {
    std::array<ayni_schema_node_v1, stream_nodes> out{};
    constexpr const char *names[]{"motor1_min",     "motor1_max",     "motor2_min", "motor2_max",
                                  "motor1_percent", "motor2_percent", "duration_ms"};
    out[0].kind = AYNI_VALUE_OBJECT;
    out[0].subtree_end = stream_nodes;
    for (uint32_t field = 1; field < stream_nodes; ++field) {
        auto &node = out[field];
        node.kind = AYNI_VALUE_I64;
        node.field_id = field;
        node.subtree_end = field + 1;
        node.flags = AYNI_SCHEMA_REQUIRED;
        node.name = motor_text(names[field - 1]);
        node.unit = motor_text(field == stream_duration ? "ms" : "percent");
        node.min_i64 =
            field == stream_duration ? motor_duration_ms_min : -int64_t(motor_percent_max);
        node.max_i64 = field == stream_duration ? motor_plan_duration_ms_max : motor_percent_max;
    }
    return out;
}
// Relational business constraints unavailable in the scalar schema. Run once on receipt.
inline bool read_motor_stream(const ayni_value_v1 &value, MotorStreamInput &out) {
    if (!value.nodes || value.node_count != stream_nodes ||
        value.nodes[0].kind != AYNI_VALUE_OBJECT) {
        return false;
    }
    uint32_t seen = 0;
    for (uint32_t at = 1; at < value.node_count; ++at) {
        const auto &node = value.nodes[at];
        if (!node.field_id || node.field_id > stream_field_count || node.kind != AYNI_VALUE_I64) {
            return false;
        }
        const auto bit = 1U << (node.field_id - 1U);
        if (seen & bit) {
            return false;
        }
        seen |= bit;
        out.fields[node.field_id - 1] = node.value.integer;
    }
    const auto &f = out.fields;
    if (f[stream_m1_min - 1] > 0 || f[stream_m1_max - 1] < 0 || f[stream_m2_min - 1] > 0 ||
        f[stream_m2_max - 1] < 0 || f[stream_m1_min - 1] < -int64_t(motor_percent_max) ||
        f[stream_m1_max - 1] > motor_percent_max ||
        f[stream_m2_min - 1] < -int64_t(motor_percent_max) ||
        f[stream_m2_max - 1] > motor_percent_max ||
        f[stream_initial_m1 - 1] < -int64_t(motor_percent_max) ||
        f[stream_initial_m1 - 1] > motor_percent_max ||
        f[stream_initial_m2 - 1] < -int64_t(motor_percent_max) ||
        f[stream_initial_m2 - 1] > motor_percent_max ||
        f[stream_duration - 1] < motor_duration_ms_min ||
        f[stream_duration - 1] > motor_plan_duration_ms_max) {
        return false;
    }
    out.initial = {int32_t(f[stream_initial_m1 - 1]), int32_t(f[stream_initial_m2 - 1]),
                   uint32_t(f[stream_duration - 1])};
    return valid_motor_command(out.initial) &&
           out.contains(out.initial.first_percent, out.initial.second_percent);
}
} // namespace wifichat
