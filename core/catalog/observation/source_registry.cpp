#include "catalog/observation/source_registry.h"
#include "catalog/abi_checks.h"

namespace ayni::catalog {
ayni_status_t validate_source(const ObservationSource &source) {
    if (source.id.size() == 0 || source.version == 0 || source.unit.size() == 0 ||
        source.validity_max_us == 0 || source.validity_max_us == UINT64_MAX ||
        (source.kind != AYNI_OBSERVATION_BOOL && source.kind != AYNI_OBSERVATION_I64) ||
        (source.origin != AYNI_OBSERVATION_MEASURED &&
         source.origin != AYNI_OBSERVATION_INFERRED)) {
        return AYNI_INVALID_ARGUMENT;
    }
    constexpr char boolean_unit[] = "bool";
    return source.kind != AYNI_OBSERVATION_BOOL ||
                   source.unit.equals({boolean_unit, sizeof(boolean_unit) - 1})
               ? AYNI_OK
               : AYNI_INVALID_ARGUMENT;
}
ayni_status_t decode_source(const ayni_observation_source_v1 &input, ObservationSource &out) {
    const auto shape = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (shape != AYNI_OK) {
        return shape;
    }
    ObservationSource value{};
    if (!value.id.assign(input.source_id) || !value.unit.assign(input.unit)) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.version = input.version;
    value.kind = input.kind;
    value.origin = input.origin;
    value.validity_max_us = input.validity_max_us;
    const auto status = validate_source(value);
    if (status == AYNI_OK) {
        out = value;
    }
    return status;
}
ayni_status_t decode_condition(const ayni_observation_condition_v1 &input,
                               ObservationCondition &out) {
    ObservationCondition value{};
    if (!value.source.assign(input.source_id) || input.source_version == 0 || input.reserved != 0 ||
        input.minimum > input.maximum || input.max_age_us == 0 || input.max_age_us == UINT64_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.version = input.source_version;
    value.minimum = input.minimum;
    value.maximum = input.maximum;
    value.max_age_us = input.max_age_us;
    out = value;
    return AYNI_OK;
}
ayni_status_t ObservationSources::add_validated(const ObservationSource &source) {
    uint32_t index = 0;
    if (find({source.id.data(), source.id.size()}, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return entries_.push_back(source) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
bool ObservationSources::find(ayni_string_v1 id, uint32_t &index) const {
    for (uint32_t at = 0; at < entries_.size(); ++at) {
        if (entries_[at].id.equals(id)) {
            index = at;
            return true;
        }
    }
    return false;
}
} // namespace ayni::catalog
