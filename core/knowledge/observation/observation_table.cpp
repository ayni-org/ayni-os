#include "knowledge/observation/observation_table.h"
#include "catalog/abi_checks.h"
#include <new>

namespace ayni::knowledge {
std::size_t ObservationTable::storage_size(uint32_t capacity) {
    return sizeof(Slot) * capacity;
}
ObservationTable::ObservationTable(const catalog::ObservationSources &sources, void *storage,
                                   uint32_t capacity)
    : sources_(sources), slots_(static_cast<Slot *>(storage)), capacity_(capacity) {
    static_assert(alignof(Slot) <= storage_alignment, "observation arena alignment");
    for (uint32_t at = 0; at < capacity_; ++at) {
        new (slots_ + at) Slot{};
    }
}
void ObservationTable::begin_boot(uint64_t boot_epoch) {
    for (uint32_t at = 0; at < capacity_; ++at) {
        slots_[at].reset();
    }
    boot_epoch_ = boot_epoch;
}
bool ObservationTable::usable(const Sample &sample, uint64_t now_us) const {
    return sample.quality == AYNI_OBSERVATION_GOOD && sample.captured_at_us <= now_us &&
           now_us < sample.valid_until_us;
}
ayni_observation_ref_v1 ObservationTable::reference(uint32_t index, const Sample &sample) const {
    const auto &source = sources_.at(index);
    return {source.id.snapshot(), source.version, boot_epoch_, sample.generation, sample.sequence};
}

// Publication never upgrades the trusted source classification. All validation
// precedes replacing the last fact, including generation/sequence and TTL checks.
ayni_status_t ObservationTable::publish(const ayni_observation_v1 &input, uint64_t now_us) {
    const auto shape = catalog::check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (!catalog::CatalogId::is_valid(input.source_id) || input.generation == 0 ||
        input.sequence == 0 || input.source_version == 0 || input.reserved != 0 ||
        input.valid_for_us == 0 || input.captured_at_us > now_us ||
        input.captured_at_us > UINT64_MAX - input.valid_for_us ||
        (input.quality != AYNI_OBSERVATION_GOOD && input.quality != AYNI_OBSERVATION_DEGRADED)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (boot_epoch_ == 0) {
        return AYNI_NOT_READY;
    }
    uint32_t index = 0;
    if (!sources_.find(input.source_id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &source = sources_.at(index);
    if (input.source_version != source.version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (input.valid_for_us > source.validity_max_us ||
        (source.kind == AYNI_OBSERVATION_BOOL && input.value != 0 && input.value != 1)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const Sample value{input.generation,     input.sequence,
                       input.captured_at_us, input.captured_at_us + input.valid_for_us,
                       input.quality,        input.value};
    if (now_us >= value.valid_until_us) {
        return AYNI_EXPIRED;
    }
    const auto &old = slots_[index];
    if (old &&
        (value.generation < old->generation ||
         (value.generation == old->generation &&
          (value.sequence <= old->sequence || value.captured_at_us < old->captured_at_us)))) {
        return AYNI_STALE;
    }
    slots_[index] = value;
    return AYNI_OK;
}

ayni_status_t ObservationTable::info(ayni_string_v1 source, uint64_t now_us,
                                     ayni_observation_info_v1 &out) const {
    uint32_t index = 0;
    if (!sources_.find(source, index) || !slots_[index]) {
        return AYNI_NOT_FOUND;
    }
    const auto &description = sources_.at(index);
    const auto &sample = *slots_[index];
    out = {sizeof(out),
           usable(sample, now_us) ? 1U : 0U,
           reference(index, sample),
           description.kind,
           description.origin,
           sample.quality,
           0,
           description.unit.snapshot(),
           sample.captured_at_us,
           sample.valid_until_us,
           sample.value};
    return AYNI_OK;
}

ayni_status_t ObservationTable::require(const catalog::ObservationCondition &condition,
                                        uint64_t now_us, ayni_observation_ref_v1 &out) const {
    uint32_t index = 0;
    if (!sources_.find({condition.source.data(), condition.source.size()}, index) ||
        !slots_[index]) {
        return AYNI_NOT_READY;
    }
    const auto &source = sources_.at(index);
    const auto &sample = *slots_[index];
    if (source.version != condition.version || source.origin != AYNI_OBSERVATION_MEASURED ||
        !usable(sample, now_us) || now_us - sample.captured_at_us > condition.max_age_us ||
        sample.value < condition.minimum || sample.value > condition.maximum) {
        return AYNI_NOT_READY;
    }
    out = reference(index, sample);
    return AYNI_OK;
}

bool ObservationTable::current(const ayni_observation_ref_v1 &ref, uint64_t now_us) const {
    uint32_t index = 0;
    if (ref.boot_epoch == 0 || ref.boot_epoch != boot_epoch_ ||
        ref.source_id.size > AYNI_CATALOG_ID_MAX_BYTES ||
        !sources_.find({ref.source_id.data, ref.source_id.size}, index) || !slots_[index]) {
        return false;
    }
    const auto &sample = *slots_[index];
    return sources_.at(index).version == ref.source_version &&
           sample.generation == ref.generation && sample.sequence == ref.sequence &&
           usable(sample, now_us);
}
} // namespace ayni::knowledge
