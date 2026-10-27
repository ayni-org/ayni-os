// Any-thread Host observation transport, separate from owner knowledge facts (ADR-0062).
#ifndef AYNI_CORE_KERNEL_INGRESS_OBSERVATION_INGRESS_H
#define AYNI_CORE_KERNEL_INGRESS_OBSERVATION_INGRESS_H
#include "foundation/bounded_array.h"
#include "kernel/runtime/ingress/event_queue.h"
#include "knowledge/observation/observation_input.h"
#include <optional>
namespace ayni::kernel {
// Queue body is an internal copied value, never a wire/storage record.
struct QueuedObservation {
    knowledge::ObservationInput input; // Already intrinsically and source validated.
    uint32_t source_index = 0;         // Immutable registered source index; zero is valid.
};
// One reserved discontinuity cell per source, independent of sample FIFO pressure.
struct ObservationGapCell {
    // Applied retains the frontier to make repeated producer reports idempotent.
    enum class Phase { pending, applied };
    knowledge::ObservationBoundary boundary; // Highest accepted gap frontier for this boot.
    Phase phase = Phase::pending;            // Owner acknowledges by applying the exact frontier.
};
// Owns producer admission, copied metadata and counters; never mutates knowledge.
class ObservationIngress {
  public:
    // Fixed arena measurement shared by create and runtime construction.
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(uint32_t capacity, uint32_t sources);
    // Fixed arena and immutable registry/Host outlive every producer and this object.
    ObservationIngress(void *storage, uint32_t capacity, uint32_t sources,
                       const catalog::ObservationSources &registry, const ayni_host_v1 &host);
    // Owner: open only after this boot reached RUNNING; old posts carry a stale epoch.
    void begin_boot(uint64_t boot_epoch);
    // Owner: close admission first, then discard each item with bounded individual locks.
    void close();
    // Any non-ISR thread. No Host calls under lock except its lock/unlock functions.
    ayni_status_t post(uint64_t boot_epoch, const knowledge::ObservationInput &input);
    // Same lifetime/thread contract; capacity reserved at source registration.
    ayni_status_t gap(uint64_t boot_epoch, const catalog::CatalogId &source, uint32_t version,
                      knowledge::ObservationBoundary boundary);
    // Owner: consume gap before dispatch; acknowledge only this copied frontier.
    bool take_gap(uint32_t source_index, knowledge::ObservationBoundary &boundary);
    uint32_t queued() const; // Snapshot count; zero while the feature is disabled.
    // Owner: take at most the entry snapshot observed at the step frontier.
    bool take(QueuedObservation &out);
    // Owner rejected a queued expired/stale/invalid value; counters use the same lock.
    void discarded();
    // Any thread: consistent diagnostic copy, no owner lifecycle reads.
    ayni_observation_ingress_info_v1 info() const;

  private:
    using Gap = std::optional<ObservationGapCell>;
    ayni_status_t admission(uint64_t boot_epoch) const; // Only while holding ingress lock.
    const catalog::ObservationSources &registry_;       // Immutable once admission opens.
    const ayni_host_v1 &host_;        // Immutable borrowed ports; no owner clock used by producers.
    const uint32_t capacity_;         // Zero disables sample and gap admission.
    std::optional<EventQueue> queue_; // Present iff capacity > 0; shared generic bounded FIFO.
    foundation::BoundedArray<Gap> gaps_; // Exactly one preconstructed cell per source slot.
    std::optional<uint64_t> boot_; // Empty when admission is closed; positive epoch otherwise.
    uint64_t accepted_ = 0;        // Successful sample enqueues since Kernel creation.
    uint64_t gap_updates_ = 0;     // Forward discontinuity changes, never repeated-marker attempts.
    uint64_t discarded_ = 0; // Owner rejection and closure drops; shared only under ingress lock.
};
} // namespace ayni::kernel
#endif
