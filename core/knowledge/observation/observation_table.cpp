#include "knowledge/observation/observation_table.h"
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
        slots_[at] = {};
    }
    boot_epoch_ = boot_epoch;
}
bool ObservationTable::usable(const Slot &slot, uint64_t now_us) const {
    const auto &sample = *slot;
    return sample.quality == AYNI_OBSERVATION_GOOD && sample.captured_at_us <= now_us &&
           now_us < sample.valid_until_us &&
           (!sample.media || now_us < sample.media->valid_until_us) &&
           (!slot.invalid_through ||
            after_boundary(sample.generation, sample.sequence, *slot.invalid_through));
}
namespace {
ayni_status_t check_clock(const ObservationInput &input, uint64_t now_us, uint64_t boot_epoch) {
    if (input.captured_at_us > now_us) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (boot_epoch == 0) {
        return AYNI_NOT_READY;
    }
    if (input.media && input.media->boot_epoch != boot_epoch) {
        return AYNI_STALE;
    }
    return AYNI_OK;
}
uint64_t saturated_add(uint64_t value, uint64_t increment) {
    return increment > UINT64_MAX - value ? UINT64_MAX : value + increment;
}
} // namespace
ayni_observation_ref_v1 ObservationTable::reference(uint32_t index, const Sample &sample) const {
    const auto &source = sources_.at(index);
    return {source.id.snapshot(), source.version, boot_epoch_, sample.generation, sample.sequence};
}

// Publication never upgrades the trusted source classification. All validation
// precedes replacing the last fact, including generation/sequence and TTL checks.
// Preserve the original owner API refusal order: future/boot, source rules,
// expiry, then sequence. Queued inputs already passed immutable source rules.
ayni_status_t ObservationTable::publish(const ObservationInput &input, uint64_t now_us) {
    const auto timed = check_clock(input, now_us, boot_epoch_);
    if (timed != AYNI_OK) {
        return timed;
    }
    uint32_t index = 0;
    if (!sources_.find({input.source.data(), input.source.size()}, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto status = check_observation_source(input, sources_.at(index));
    return status == AYNI_OK ? publish_current(input, index, now_us) : status;
}
ayni_status_t ObservationTable::publish_from_ingress(const ObservationInput &input, uint32_t index,
                                                     uint64_t now_us) {
    const auto timed = check_clock(input, now_us, boot_epoch_);
    return timed == AYNI_OK ? publish_current(input, index, now_us) : timed;
}
ayni_status_t ObservationTable::publish_current(const ObservationInput &input, uint32_t index,
                                                uint64_t now_us) {
    if (now_us >= input.captured_at_us + input.valid_for_us ||
        (input.media && now_us >= input.media->valid_until_us)) {
        return AYNI_EXPIRED;
    }
    return replace_sample(input, index);
}
// Only owner serial execution reaches this point: an invalidated frontier cannot
// be revived by an old queued result, even while its diagnostic sample remains.
ayni_status_t ObservationTable::replace_sample(const ObservationInput &input, uint32_t index) {
    auto &slot = slots_[index];
    if (slot.invalid_through &&
        !after_boundary(input.generation, input.sequence, *slot.invalid_through)) {
        return AYNI_STALE;
    }
    if (slot &&
        (input.generation < slot->generation ||
         (input.generation == slot->generation &&
          (input.sequence <= slot->sequence || input.captured_at_us < slot->captured_at_us)))) {
        return AYNI_STALE;
    }
    // A newly accepted sample ends the gap's unusable interval. Keep the floor
    // so a later old tail cannot become usable after a generation change.
    const auto before = known_through(slot, input.generation);
    if (before != 0 && input.sequence > before && input.sequence - before > 1) {
        slot.gaps = saturated_add(slot.gaps, input.sequence - before - 1);
    }
    slot.sample = Sample{input.generation,     input.sequence,
                         input.captured_at_us, input.captured_at_us + input.valid_for_us,
                         input.quality,        input.value,
                         input.media};
    return AYNI_OK;
}
uint64_t ObservationTable::known_through(const Slot &slot, uint64_t generation) {
    const auto observed = slot && slot->generation == generation ? slot->sequence : 0;
    const auto invalid = slot.invalid_through && slot.invalid_through->generation == generation
                             ? slot.invalid_through->through
                             : 0;
    return observed > invalid ? observed : invalid;
}
void ObservationTable::invalidate(uint32_t index, ObservationBoundary through) {
    auto &slot = slots_[index];
    // The current generation already rejects this old tail. Its historical
    // sequences cannot be counted again without retaining unbounded history (C-OBS-003).
    if (slot && through.generation < slot->generation) {
        return;
    }
    if (slot.invalid_through &&
        !after_boundary(through.generation, through.through, *slot.invalid_through)) {
        return;
    }
    const auto before = known_through(slot, through.generation);
    if (through.through > before) {
        slot.gaps = saturated_add(slot.gaps, through.through - before);
    }
    slot.invalid_through = through;
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
           usable(slots_[index], now_us) ? 1U : 0U,
           reference(index, sample),
           description.kind,
           description.origin,
           sample.quality,
           0,
           description.unit.snapshot(),
           sample.captured_at_us,
           sample.valid_until_us,
           sample.value,
           sample.media.value_or(ayni_media_ref_v1{}),
           slots_[index].gaps};
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
        !usable(slots_[index], now_us) || now_us - sample.captured_at_us > condition.max_age_us ||
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
           usable(slots_[index], now_us);
}
} // namespace ayni::knowledge
