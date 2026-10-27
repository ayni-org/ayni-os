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
    static std::size_t storage_size(uint32_t capacity, uint32_t subscriptions = 0);
    // Source registry and caller-owned arena outlive this table.
    ObservationTable(const catalog::ObservationSources &sources, void *storage, uint32_t capacity,
                     uint32_t subscriptions = 0);
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
    // Bind one Call slot to its Task/source before Provider submission; a fresh
    // generation drops old sample validity without changing Host-source facts.
    void begin_subscription(uint32_t call_index, uint32_t source_index, uint64_t task_id,
                            uint64_t subscription_id, uint64_t generation);
    // Owner projects the confirmed lease into knowledge; no callback sees domain internals.
    void activate_subscription(uint32_t call_index, uint64_t generation, uint64_t valid_until_us);
    // Close immediately refuses every dependency, retaining the last sample as diagnostics.
    void close_subscription(uint32_t call_index);
    // Validated transport metadata is still checked against the current Task opening and lease.
    ayni_status_t publish_subscription(const ObservationInput &input, uint32_t call_index,
                                       uint64_t subscription_id, uint64_t now_us);
    // An old queued gap cannot invalidate a later opening in a reused Call slot.
    void invalidate_subscription(uint32_t call_index, uint64_t generation,
                                 ObservationBoundary through);
    // Trusted owner copy for a specific Task-owned subscription; no cross-Task source alias.
    ayni_status_t subscription_info(uint64_t subscription_id, uint64_t now_us,
                                    ayni_observation_info_v1 &out) const;
    // Diagnostic copy may describe expired/degraded facts; usable remains explicit.
    ayni_status_t info(ayni_string_v1 source, uint64_t now_us, ayni_observation_info_v1 &out) const;
    // A Scene requires a current GOOD measurement in its finite interval and age.
    // Success returns the exact observation identity, never just its source name.
    ayni_status_t require(const catalog::ObservationCondition &condition, uint64_t now_us,
                          ayni_observation_ref_v1 &out, uint64_t task_id = 0) const;
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
    // Immutable association plus an optional confirmed live lease. The copy is
    // necessary for current-boot knowledge lifetime; CallTable owns durable intent.
    struct Scope {
        uint32_t source_index;    // Static registered source, below source_capacity_.
        uint64_t task_id;         // Positive owning Task, never a table position.
        uint64_t subscription_id; // Positive stable Call ID.
        uint64_t generation;      // Exact opening; all its samples must match.
        std::optional<uint64_t>
            until; // Empty means closed; otherwise finite exclusive lease deadline.
    };
    // Host sources and Task-scoped subscriptions never share a sample cell.
    struct Slot {
        std::optional<Scope> scope; // Present only in a bound subscription cell.
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
    // Resolve copied association to immutable source; Host slots use their fixed position.
    uint32_t source_of(uint32_t slot_index) const;
    // Common diagnostic writer for the two public lookup paths.
    void describe(uint32_t slot_index, uint64_t now_us, ayni_observation_info_v1 &out) const;
    // Task scope wins over Host fallback, including a closed/missing Task sample.
    uint32_t slot_for(uint32_t source_index, uint64_t task_id) const;
    // Both paths check expiry only after source rules, preserving owner refusal order.
    ayni_status_t publish_current(const ObservationInput &input, uint32_t index, uint64_t now_us);
    // Only after owner time and immutable source validation; publish one whole fact.
    ayni_status_t replace_sample(const ObservationInput &input, uint32_t index);
    // Build the exact dependency identity from immutable source and owner facts.
    ayni_observation_ref_v1 reference(uint32_t index, const Sample &sample) const;
    const catalog::ObservationSources &sources_; // Borrowed immutable source metadata.
    Slot *slots_;              // Sole latest facts, one per configured source slot.
    uint32_t source_capacity_; // Host-source prefix; configured registry bound.
    uint32_t capacity_;        // Host prefix plus fixed Call-scoped cells.
    uint64_t boot_epoch_ = 0;  // Zero before begin_boot; never restored from an observation.
};
} // namespace ayni::knowledge
#endif
