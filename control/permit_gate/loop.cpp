#include "gate.h"
#include <algorithm>
#include <ayni/control_loop.h>
#include <climits>

namespace {
using namespace ayni::control;
constexpr uint32_t full_scale = AYNI_CONTROL_LOOP_FULL_SCALE;
const ayni_control_value_v1 *field(const ayni_control_segment_v1 &segment,
                                   const ayni_catalog_name_v1 &id) {
    for (uint32_t at = 0; at < segment.field_count; ++at) {
        if (compare_names(segment.fields[at].field_id, id) == 0) {
            return &segment.fields[at];
        }
    }
    return nullptr;
}
// Unsigned magnitude represents INT64_MIN without negation overflow.
uint64_t magnitude(int64_t value) {
    return value < 0 ? uint64_t(0) - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
}
int64_t signed_magnitude(uint64_t value, bool negative) {
    if (negative && value == (uint64_t(1) << (sizeof(int64_t) * CHAR_BIT - 1U))) {
        return INT64_MIN;
    }
    return negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
}
// floor(100*n/d) for n<d, using a bounded search of safe ceil(d*percent/100).
// Quotient/remainder decomposition never multiplies an unbounded 64-bit value by 100.
uint32_t percent(uint64_t n, uint64_t d) {
    uint32_t low = 0, high = full_scale;
    while (low < high) {
        const auto mid = low + (high - low + 1U) / 2U;
        const auto threshold =
            (d / full_scale) * mid + ((d % full_scale) * mid + full_scale - 1U) / full_scale;
        if (n >= threshold) {
            low = mid;
        } else {
            high = mid - 1U;
        }
    }
    return low;
}
ayni_status_t validate(const ayni_control_loop_t &loop, const ayni_control_segment_v1 &desired) {
    const auto shape = structure(loop.struct_size, sizeof(loop));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (structure(desired.struct_size, sizeof(desired)) != AYNI_OK || !loop.field_count ||
        loop.field_count > AYNI_CONTROL_FIELD_COUNT_MAX || !desired.field_count ||
        desired.field_count > AYNI_CONTROL_FIELD_COUNT_MAX ||
        loop.sensor_count > AYNI_CONTROL_LOOP_SENSORS_MAX || loop.has_previous > 1U ||
        !loop.max_cycle_gap_us ||
        (loop.has_previous && (loop.previous.struct_size != sizeof(loop.previous) ||
                               loop.previous.field_count > AYNI_CONTROL_FIELD_COUNT_MAX))) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < desired.field_count; ++at) {
        const auto &value = desired.fields[at];
        if (!valid_name(value.field_id) || !valid_name(value.unit)) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t earlier = 0; earlier < at; ++earlier) {
            if (!compare_names(value.field_id, desired.fields[earlier].field_id)) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    }
    for (uint32_t at = 0; at < loop.field_count; ++at) {
        if (!valid_name(loop.fields[at]) || !field(desired, loop.fields[at])) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t earlier = 0; earlier < at; ++earlier) {
            if (!compare_names(loop.fields[at], loop.fields[earlier])) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    }
    for (uint32_t at = 0; at < loop.sensor_count; ++at) {
        const auto &sensor = loop.sensors[at];
        if (!sensor.source_id || sensor.reserved || !sensor.max_age_us ||
            sensor.slow_value <= sensor.stop_value) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t earlier = 0; earlier < at; ++earlier) {
            if (sensor.source_id == loop.sensors[earlier].source_id) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    }
    return AYNI_OK;
}
// Required sensor scale is the tightest channel; comparisons precede unsigned differences.
uint32_t sensor_scale(const ayni_control_loop_t &loop, const ayni_control_sensor_sample_v1 *samples,
                      uint32_t count, uint64_t now) {
    uint32_t scale = full_scale;
    for (uint32_t at = 0; at < loop.sensor_count; ++at) {
        const auto &limit = loop.sensors[at];
        const ayni_control_sensor_sample_v1 *found = nullptr;
        for (uint32_t find = 0; find < count; ++find) {
            if (samples[find].source_id == limit.source_id) {
                if (found) {
                    return 0;
                }
                found = &samples[find];
            }
        }
        if (!found || found->measured_ok != 1U || found->captured_at_us > now ||
            now - found->captured_at_us > limit.max_age_us || found->value <= limit.stop_value) {
            return 0;
        }
        if (found->value < limit.slow_value) {
            scale =
                std::min(scale, percent(uint64_t(found->value) - uint64_t(limit.stop_value),
                                        uint64_t(limit.slow_value) - uint64_t(limit.stop_value)));
        }
    }
    return scale;
}
} // namespace

extern "C" ayni_status_t
ayni_control_loop_limit(const ayni_control_loop_t *loop, const ayni_control_segment_v1 *desired,
                        const ayni_control_sensor_sample_v1 *sensors, uint32_t sensor_count,
                        uint64_t now_us, uint32_t hold_scale_percent,
                        ayni_control_segment_v1 *out_effective,
                        ayni_control_loop_action_t *out_action, ayni_control_loop_t *out_next) {
    if (out_action) {
        *out_action = AYNI_LOOP_SOFT_STOP;
    }
    if (!loop || !desired || !out_effective || !out_action || !out_next ||
        (sensor_count && !sensors) || sensor_count > AYNI_CONTROL_LOOP_SENSORS_MAX ||
        hold_scale_percent > full_scale) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto checked = validate(*loop, *desired);
    if (checked != AYNI_OK) {
        return checked;
    }
    auto effective = *desired;
    auto scale = std::min(hold_scale_percent, sensor_scale(*loop, sensors, sensor_count, now_us));
    const bool overdue =
        loop->has_previous &&
        (now_us < loop->last_step_us || now_us - loop->last_step_us > loop->max_cycle_gap_us);
    if (overdue) {
        scale = 0;
    }
    for (uint32_t at = 0; at < effective.field_count; ++at) {
        auto &value = effective.fields[at];
        bool controlled = false;
        for (uint32_t find = 0; find < loop->field_count; ++find) {
            controlled = controlled || !compare_names(value.field_id, loop->fields[find]);
        }
        if (!controlled) {
            continue;
        }
        const auto magnitude_desired = magnitude(value.value);
        auto reduced = (magnitude_desired / full_scale) * scale +
                       ((magnitude_desired % full_scale) * scale) / full_scale;
        const auto *previous = loop->has_previous ? field(loop->previous, value.field_id) : nullptr;
        if (loop->slew_per_step && scale) {
            const auto prior = previous && (previous->value < 0) == (value.value < 0)
                                   ? magnitude(previous->value)
                                   : 0;
            if (reduced > prior && reduced - prior > loop->slew_per_step) {
                reduced = prior + loop->slew_per_step;
            }
        }
        value.value = signed_magnitude(reduced, value.value < 0);
    }
    // Publish explicit next state last: loop/out_next aliasing cannot alter this calculation.
    *out_next = *loop;
    out_next->last_step_us = now_us;
    out_next->has_previous = 1;
    out_next->previous = effective;
    *out_effective = effective;
    *out_action = scale ? AYNI_LOOP_APPLY : AYNI_LOOP_SOFT_STOP;
    return AYNI_OK;
}
