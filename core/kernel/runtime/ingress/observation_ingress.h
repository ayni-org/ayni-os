// Any-thread Host observation transport, separate from owner knowledge facts (ADR-0062).
#ifndef AYNI_CORE_KERNEL_INGRESS_OBSERVATION_INGRESS_H
#define AYNI_CORE_KERNEL_INGRESS_OBSERVATION_INGRESS_H
#include "foundation/bounded_array.h"
#include "kernel/ports/subscription_ingress_port.h"
#include "kernel/runtime/ingress/event_queue.h"
#include "knowledge/observation/observation_input.h"
#include <ayni/subscriptions.h>
#include <optional>
namespace ayni::kernel {
class ObservationIngress;
}
// Address is immutable; the reference in each post identifies a collection incarnation.
struct ayni_subscription_sink {
    ayni::kernel::ObservationIngress *pool; // Borrowed until every producer has joined.
    uint32_t index;                         // Fixed Call slot, not an authorization token.
};
namespace ayni::kernel {
// Queue body is an internal copied value, never a wire/storage record.
struct ScopedObservation {
    ayni_subscription_ref_v1 reference; // Copied exact call/boot/generation before queueing.
    uint32_t call_index; // Stable Call slot for this transport lifetime; owner rechecks identity.
};
struct QueuedObservation {
    knowledge::ObservationInput input;        // Already intrinsically and source validated.
    std::optional<ScopedObservation> scope{}; // Empty for privileged Host-source publication.
    uint32_t source_index = 0;                // Immutable registered source index; zero is valid.
};
// One reserved discontinuity cell per source, independent of sample FIFO pressure.
struct ObservationGapCell {
    // Applied retains the frontier to make repeated producer reports idempotent.
    enum class Phase { pending, applied };
    knowledge::ObservationBoundary boundary; // Highest accepted gap frontier for this boot.
    Phase phase = Phase::pending;            // Owner acknowledges by applying the exact frontier.
};
// Locked projection for one Task-owned collection, separate from durable Call facts.
struct ObservationSubscriptionBinding {
    ayni_subscription_ref_v1 reference; // Whole exact opening identity.
    uint32_t source_index;              // Copied immutable source index, no registry lookup race.
    uint64_t owner;                     // Media must belong to this original Agent instance.
    std::optional<uint64_t>
        until; // Empty means gate closed; owner alone installs confirmed deadlines.
};
struct ObservationSubscriptionCell {
    ayni_subscription_sink target{}; // Immutable pool/index address across incarnations.
    std::optional<ObservationSubscriptionBinding> binding; // None before first prepare.
    std::optional<ObservationGapCell> gap; // Independent reliable gap frontier for this opening.
};
// Owns producer admission, copied metadata and counters; never mutates knowledge.
class ObservationIngress {
  public:
    // Fixed arena measurement shared by create and runtime construction.
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(uint32_t capacity, uint32_t sources,
                                    uint32_t subscriptions = 0);
    // Fixed arena and immutable registry/Host outlive every producer and this object.
    ObservationIngress(void *storage, uint32_t capacity, uint32_t sources,
                       const catalog::ObservationSources &registry, const ayni_host_v1 &host,
                       uint32_t subscriptions = 0);
    // Fixed owner-only capability for CallRuntime; service never reads transport internals.
    SubscriptionIngressPort subscription_port();
    // Owner prepares a closed exact opening before submit; old borrow must already be returned.
    ayni_subscription_sink_t *prepare_subscription(uint32_t call_index,
                                                   ayni_subscription_ref_v1 reference,
                                                   uint32_t source_index, uint64_t owner);
    // Confirmed lease projection; no implicit extension from result arrival time.
    bool activate_subscription(uint32_t call_index, ayni_subscription_ref_v1 reference,
                               uint64_t until);
    // Closing stops producers immediately; queued values still undergo owner identity checks.
    void close_subscription(uint32_t call_index, ayni_subscription_ref_v1 reference);
    // Stable target shape/address check, independent of mutable session state.
    bool owns(const ayni_subscription_sink_t *sink) const;
    // Any-thread scope checks and FIFO copy; successful calls wake after releasing the lock.
    ayni_status_t post_subscription(uint32_t call_index, ayni_subscription_ref_v1 reference,
                                    const knowledge::ObservationInput &input);
    // Same reliable per-opening frontier, independent of Host-source gap cells.
    ayni_status_t subscription_gap(uint32_t call_index, ayni_subscription_ref_v1 reference,
                                   uint64_t through);
    // Owner consumes at most one pending marker from each Call slot per step.
    bool take_subscription_gap(uint32_t call_index, ayni_subscription_ref_v1 &reference,
                               knowledge::ObservationBoundary &boundary);
    // Fixed scan bound; zero when the whole ingress feature is disabled.
    uint32_t subscription_capacity() const { return sessions_.size(); }
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
    // Shared placement calculation for measurement and construction.
    static std::size_t sessions_offset(uint32_t capacity, uint32_t sources);
    // Caller already holds the lock and checked the class-specific identity/source rules.
    ayni_status_t enqueue(const QueuedObservation &value);
    // Caller holds ingress lock; classify a target before reading its copied binding.
    ayni_status_t subscription_admission(uint32_t index, ayni_subscription_ref_v1 reference) const;
    using Gap = std::optional<ObservationGapCell>;
    ayni_status_t admission(uint64_t boot_epoch) const; // Only while holding ingress lock.
    const catalog::ObservationSources &registry_;       // Immutable once admission opens.
    const ayni_host_v1 &host_;        // Immutable borrowed ports; no owner clock used by producers.
    const uint32_t capacity_;         // Zero disables sample and gap admission.
    std::optional<EventQueue> queue_; // Present iff capacity > 0; shared generic bounded FIFO.
    foundation::BoundedArray<ObservationSubscriptionCell>
        sessions_;                       // One opaque sink/gap per Call.
    foundation::BoundedArray<Gap> gaps_; // Exactly one preconstructed cell per source slot.
    std::optional<uint64_t> boot_; // Empty when admission is closed; positive epoch otherwise.
    uint64_t accepted_ = 0;        // Successful sample enqueues since Kernel creation.
    uint64_t gap_updates_ = 0;     // Forward discontinuity changes, never repeated-marker attempts.
    uint64_t discarded_ = 0; // Owner rejection and closure drops; shared only under ingress lock.
};
} // namespace ayni::kernel
#endif
