// Current-boot observations, validity and dependency identity. No collection I/O.
#ifndef AYNI_CORE_KNOWLEDGE_OBSERVATION_TABLE_H
#define AYNI_CORE_KNOWLEDGE_OBSERVATION_TABLE_H
#include "catalog/observation/source_registry.h"
#include "knowledge/observation/observation_input.h"
#include <optional>

namespace ayni::knowledge {
// Observation facts belong to this owner; source sequence numbers are supplied
// by the collection owner and only checked here, never generated or incremented.
class ObservationTable {
  public:
    // Fixed arena alignment and footprint, no allocation during publication.
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(uint32_t capacity);
    // Source registry and caller-owned arena outlive this table.
    ObservationTable(const catalog::ObservationSources &sources, void *storage, uint32_t capacity);
    // New Kernel boot invalidates all physical observations; no replay revalidates them.
    void begin_boot(uint64_t boot_epoch);
    // Intrinsic/source fields were checked once by the ingress boundary. Only
    // owner time, current boot, ordering and an applied gap remain mutable here.
    ayni_status_t publish(const ObservationInput &input, uint64_t now_us);
    // Queue already checked immutable source rules before copying.
    ayni_status_t publish_from_ingress(const ObservationInput &input, uint32_t source_index,
                                       uint64_t now_us);
    // Applied discontinuity preserves diagnostic data but makes old dependencies unusable.
    void invalidate(uint32_t source_index, ObservationBoundary through);
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
        ayni_observation_quality_t quality;     // GOOD or DEGRADED, never an authorization fact.
        int64_t value;                          // BOOL 0/1 or registered-unit I64.
        std::optional<ayni_media_ref_v1> media; // Opaque metadata owned with the sample, no bytes.
    };
    // Each source has at most one complete observation; no sentinel scalar value.
    struct Slot {
        std::optional<Sample>
            sample; // Last accepted value, retained even across a gap for diagnostics.
        std::optional<ObservationBoundary> invalid_through; // Explicit invalid source frontier.
        uint64_t gaps = 0; // Count of lost sequences, independent of the latest diagnostic value.
        // Optional-style inspection keeps the retained sample representation private.
        explicit operator bool() const { return sample.has_value(); }
        // Borrow the present sample only within one owner call.
        const Sample *operator->() const { return &*sample; }
        // Require presence, already decided by the caller.
        const Sample &operator*() const { return *sample; }
    };
    // One shared frontier prevents counting already observed or invalidated sequences twice.
    static uint64_t known_through(const Slot &slot, uint64_t generation);
    // Sample/media expiry and explicit gaps all prevent a dependency from authorizing work.
    bool usable(const Slot &slot, uint64_t now_us) const;
    // Both paths check expiry only after source rules, preserving owner refusal order.
    ayni_status_t publish_current(const ObservationInput &input, uint32_t index, uint64_t now_us);
    // Only after owner time and immutable source validation; publish one whole fact.
    ayni_status_t replace_sample(const ObservationInput &input, uint32_t index);
    // Build the exact dependency identity from immutable source and owner facts.
    ayni_observation_ref_v1 reference(uint32_t index, const Sample &sample) const;
    const catalog::ObservationSources &sources_; // Borrowed immutable source metadata.
    Slot *slots_;             // Sole latest facts, one per configured source slot.
    uint32_t capacity_;       // Arena bound, equal to source registry capacity.
    uint64_t boot_epoch_ = 0; // Zero before begin_boot; never restored from an observation.
};
} // namespace ayni::knowledge
#endif
