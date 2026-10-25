// Current-boot observations, validity and dependency identity. No collection I/O.
#ifndef AYNI_CORE_KNOWLEDGE_OBSERVATION_TABLE_H
#define AYNI_CORE_KNOWLEDGE_OBSERVATION_TABLE_H
#include "catalog/observation/source_registry.h"
#include <optional>

namespace ayni::knowledge {
// Observation facts belong to this owner; source sequence numbers are supplied
// by the collection owner and only checked here, never generated or incremented.
class ObservationTable {
  public:
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(uint32_t capacity);
    // Source registry and caller-owned arena outlive this table.
    ObservationTable(const catalog::ObservationSources &sources, void *storage, uint32_t capacity);
    // New Kernel boot invalidates all physical observations; no replay revalidates them.
    void begin_boot(uint64_t boot_epoch);
    // Typed boundary checks and atomic latest-value replacement; failure preserves facts.
    ayni_status_t publish(const ayni_observation_v1 &input, uint64_t now_us);
    // Diagnostic copy may describe expired/degraded facts; usable remains explicit.
    ayni_status_t info(ayni_string_v1 source, uint64_t now_us, ayni_observation_info_v1 &out) const;
    // A Scene requires a current GOOD measurement in its finite interval and age.
    // Success returns the exact observation identity, never just its source name.
    ayni_status_t require(const catalog::ObservationCondition &condition, uint64_t now_us,
                          ayni_observation_ref_v1 &out) const;
    // Exact dependency still current and usable; an updated source invalidates old versions.
    bool current(const ayni_observation_ref_v1 &reference, uint64_t now_us) const;

  private:
    // Complete latest fact, paired identity and capture/expiry times.
    struct Sample {
        uint64_t generation;     // Positive generation supplied by collection owner.
        uint64_t sequence;       // Positive per-generation source sequence.
        uint64_t captured_at_us; // Local monotonic timestamp, never future at acceptance.
        uint64_t valid_until_us; // Exclusive capture + duration, no overflow.
        ayni_observation_quality_t quality; // GOOD or DEGRADED, never an authorization fact.
        int64_t value;                      // BOOL 0/1 or registered-unit I64.
    };
    // Each source has at most one complete observation; no sentinel scalar value.
    using Slot = std::optional<Sample>;
    bool usable(const Sample &sample, uint64_t now_us) const;
    ayni_observation_ref_v1 reference(uint32_t index, const Sample &sample) const;
    const catalog::ObservationSources &sources_; // Borrowed immutable source metadata.
    Slot *slots_;             // Sole latest facts, one per configured source slot.
    uint32_t capacity_;       // Arena bound, equal to source registry capacity.
    uint64_t boot_epoch_ = 0; // Zero before begin_boot; never restored from an observation.
};
} // namespace ayni::knowledge
#endif
