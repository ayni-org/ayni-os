#include "gate.h"
#include <cstring>

namespace ayni::control {
// Local identity parsing is independent of Core's private catalog. This gate
// must validate its C boundary even when no Kernel is linked (ADR-0049).
bool valid_name(const ayni_catalog_name_v1 &name) {
    if (name.size == 0 || name.size > AYNI_CATALOG_ID_MAX_BYTES) {
        return false;
    }
    for (uint32_t at = 0; at < name.size; ++at) {
        const char value = name.data[at];
        if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' ||
              value == '_' || value == '-')) {
            return false;
        }
    }
    return true;
}
int compare_names(const ayni_catalog_name_v1 &left, const ayni_catalog_name_v1 &right) {
    const auto common = left.size < right.size ? left.size : right.size;
    const auto bytes = std::memcmp(left.data, right.data, common);
    return bytes != 0 ? bytes : (left.size < right.size ? -1 : left.size != right.size ? 1 : 0);
}
namespace {
void clear_tail(ayni_catalog_name_v1 &name) {
    std::memset(name.data + name.size, 0, AYNI_CATALOG_ID_MAX_BYTES - name.size);
}
} // namespace

ayni_status_t structure(uint32_t actual, uint32_t expected) {
    return actual == expected  ? AYNI_OK
           : actual > expected ? AYNI_UNSUPPORTED_VERSION
                               : AYNI_INVALID_ARGUMENT;
}
bool valid_epochs(const ayni_permit_epochs_v1 &epochs) {
    return epochs.kernel_boot_epoch != 0 && epochs.authority_epoch != 0 &&
           epochs.executor_boot_epoch != 0 && epochs.safety_epoch != 0 &&
           epochs.calibration_version != 0 && epochs.reserved == 0;
}
bool same_epochs(const ayni_permit_epochs_v1 &left, const ayni_permit_epochs_v1 &right) {
    return left.kernel_boot_epoch == right.kernel_boot_epoch &&
           left.authority_epoch == right.authority_epoch &&
           left.executor_boot_epoch == right.executor_boot_epoch &&
           left.safety_epoch == right.safety_epoch &&
           left.calibration_version == right.calibration_version;
}

// Shape, identity, ranges and canonical order have one owner for both startup
// device limits and every accepted permit; unused caller bytes never survive.
ayni_status_t normalize_envelope(const ayni_control_envelope_v1 &input,
                                 ayni_control_envelope_v1 &out) {
    if (input.count == 0 || input.count > AYNI_CONTROL_FIELD_COUNT_MAX || input.reserved != 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_control_envelope_v1 value{};
    value.count = input.count;
    for (uint32_t index = 0; index < input.count; ++index) {
        auto range = input.fields[index];
        if (!valid_name(range.field_id) || !valid_name(range.unit) ||
            range.minimum > range.maximum) {
            return AYNI_INVALID_ARGUMENT;
        }
        clear_tail(range.field_id);
        clear_tail(range.unit);
        uint32_t at = index;
        while (at != 0 && compare_names(range.field_id, value.fields[at - 1].field_id) < 0) {
            value.fields[at] = value.fields[at - 1];
            --at;
        }
        if (at != 0 && compare_names(range.field_id, value.fields[at - 1].field_id) == 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        value.fields[at] = range;
    }
    out = value;
    return AYNI_OK;
}

bool within(const ayni_control_envelope_v1 &approved, const ayni_control_envelope_v1 &limits) {
    if (approved.count != limits.count) {
        return false;
    }
    for (uint32_t at = 0; at < approved.count; ++at) {
        const auto &range = approved.fields[at];
        const auto &limit = limits.fields[at];
        if (compare_names(range.field_id, limit.field_id) != 0 ||
            compare_names(range.unit, limit.unit) != 0 || range.minimum < limit.minimum ||
            range.maximum > limit.maximum) {
            return false;
        }
    }
    return true;
}

// Sample order is immaterial, but every accepted field must occur exactly once.
// No truncation, implicit defaults, unit conversion or clamping is permitted.
bool valid_sample(const ayni_control_sample_v1 &sample, const ayni_execution_permit_v1 &permit) {
    if (sample.count != permit.envelope.count) {
        return false;
    }
    std::array<bool, AYNI_CONTROL_FIELD_COUNT_MAX> seen{};
    for (uint32_t at = 0; at < sample.count; ++at) {
        const auto &value = sample.fields[at];
        if (!valid_name(value.field_id) || !valid_name(value.unit)) {
            return false;
        }
        uint32_t found = 0;
        for (; found < permit.envelope.count; ++found) {
            if (compare_names(value.field_id, permit.envelope.fields[found].field_id) == 0) {
                break;
            }
        }
        if (found == permit.envelope.count || seen[found]) {
            return false;
        }
        const auto &range = permit.envelope.fields[found];
        if (compare_names(value.unit, range.unit) != 0 || value.value < range.minimum ||
            value.value > range.maximum) {
            return false;
        }
        seen[found] = true;
    }
    return true;
}
} // namespace ayni::control
