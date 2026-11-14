// Side-effect knowledge and the durable dispatch barrier (C-COMMIT-006/007).
// This owner contains no Provider callback, Task state or resource occupancy.
#ifndef AYNI_CORE_WORK_ACTION_ACTION_RECORD_H
#define AYNI_CORE_WORK_ACTION_ACTION_RECORD_H

#include "catalog/catalog_id.h"
#include "foundation/transition_stamp.h"

#include <ayni/ayni.h>
#include <cstdint>
#include <optional>

namespace ayni::work {

// Durable knowledge, independent of transport cancellation or Task lifecycle.
enum class ActionPhase : uint32_t {
    empty = 0, // Inert construction only; never a valid persisted fact.
    prepared = 1,
    dispatching,
    in_progress,
    completed,
    failed,
    cancelled,
    result_unknown,
};

// The owner that applied a fact and its transition in the device journal.
struct ActionStamp {
    uint64_t boot_epoch;                    // Positive writer boot, never a monotonic clock.
    foundation::TransitionStamp transition; // Positive transition ID and commit sequence.
};

// Immutable operation identity; the logical call owns its Task, step and input.
struct ActionIdentity {
    uint64_t operation_id;         // Stable Kernel ID; never changed to retry this operation.
    uint64_t logical_call_id;      // Stable foreign key; binding changes need a new operation.
    catalog::CatalogId binding_id; // Exact registered binding, nonempty.
    uint32_t binding_version;      // Positive exact version, fixed before preparation.
};

// Explicit evidence a result carries; failure alone never means no effect.
enum class ActionEffect : uint32_t { none = 1, partial, complete, unknown };

// Keep the source of a conclusion: known failure with no effects is not proof
// that an old operation was authoritatively never accepted (C-CALL-008/010).
enum class ActionEvidence : uint32_t {
    none = 0,
    local_non_delivery,
    provider_result,
    authoritative_result,
    authoritative_not_accepted,
};

// Complete persisted fact, validated by this owner before installation.
struct ActionFact {
    ActionIdentity identity; // Immutable across every state transition.
    ActionPhase phase;       // One knowledge state; no independent terminal boolean.
    ActionEffect effect;     // NONE before possible delivery; UNKNOWN when unresolved.
    ActionEvidence evidence; // NONE before resolution; constrained by phase/effect.
    ActionStamp prepared;    // First preparation fact, retained for audit.
    ActionStamp latest;      // Most recent state fact, >= prepared within journal order.
};

// A bounded value, held by the eventual action table. One logical owner calls
// every method. It admits local transitions but never makes them durable:
// the coordinator must journal each successful change in its own transition.
class ActionRecord {
  public:
    // Inert storage for in-place construction; only prepare installs a fact.
    // Fixed table slots must not copy a pre-claim snapshot back over live state.
    ActionRecord() = default;
    ActionRecord(const ActionRecord &) = delete;
    ActionRecord &operator=(const ActionRecord &) = delete;
    ActionRecord(ActionRecord &&) = delete;
    ActionRecord &operator=(ActionRecord &&) = delete;
    // Schema/authority/resource admission is a caller prerequisite. Failure
    // changes no output; IDs, binding and the initial stamp are checked here.
    // out must be empty. The table/allocator owns global operation-ID uniqueness.
    static ayni_status_t prepare(const ActionIdentity &identity, ActionStamp stamp,
                                 std::optional<ActionRecord> &out);
    // One validator for live facts and the future persisted record reader.
    static bool valid(const ActionFact &fact);
    // Installs an already validated replay fact; prior DISPATCHING is sealed
    // against submission. Only a never-dispatched PREPARED fact may be admitted anew.
    void restore_validated(const ActionFact &fact);
    // Immutable view until the next successful transition; valid only after prepare.
    const ActionFact &fact() const { return fact_; }

    // Enters the possible-delivery boundary after PREPARED is queued; claim still waits for the
    // complete durable prefix.
    ayni_status_t begin_dispatch(ActionStamp stamp);
    // Owner calls exactly once immediately before submit, after final authority
    // checks. Refuses until this boot's DISPATCHING fact is durable. A claim is
    // volatile: future replay must reconcile DISPATCHING, never re-claim it.
    bool claim_dispatch(uint64_t current_boot, uint64_t durable_seq);
    // Definitely not handed to a Provider (final check failed), or submit
    // returned NOT_RETAINED. Those are separate owner-proven entry points.
    ayni_status_t cancel_before_delivery(ActionStamp stamp);
    ayni_status_t rejected_not_retained(ActionStamp stamp);
    // A normal Provider receipt from the exact claimed attempt, already matched
    // by work/call. Progress does not imply completion or absence of effects.
    ayni_status_t progress(ActionStamp stamp);
    ayni_status_t complete(ActionStamp stamp);
    // A known failure can retain partial or unknown effects. NONE requires
    // affirmative evidence from the declared Provider contract, not HTTP status.
    ayni_status_t fail(ActionEffect effect, ActionStamp stamp);
    // Provider confirms cancellation of the claimed attempt. Effects may be
    // partial/unknown; this neither proves non-delivery nor actuator stop.
    ayni_status_t cancelled(ActionEffect effect, ActionStamp stamp);
    // The attempt ended without a definite result. No retry path leaves UNKNOWN.
    ayni_status_t abandon(ActionStamp stamp);

    // Terminal outcome or effect still needs authoritative reconciliation.
    static bool unresolved(const ActionFact &fact);
    // Authoritative query evidence is usable only once old delivery is drained
    // or isolated. BEST_EFFORT/missing records cannot create these proofs; the
    // coordinator checks capability declarations and transport disposition.
    enum class OldAttempt { drained, isolated };
    enum class QueryConclusion { completed, failed, not_accepted };
    ayni_status_t reconcile(QueryConclusion conclusion, ActionEffect effect, OldAttempt disposition,
                            ActionStamp stamp);

  private:
    // An accepted local claim ends when a terminal result is applied. Cancel
    // control alone never changes it; work/call owns actual transport draining.
    enum class Delivery { unclaimed, claimed, finished, restored_prepared, restored };
    bool advances(ActionStamp stamp) const;
    bool receiving() const;
    ayni_status_t finish(ActionPhase phase, ActionEffect effect, ActionEvidence evidence,
                         ActionStamp stamp);
    ActionFact fact_{};                       // Phase empty until prepare; then owned knowledge.
    Delivery delivery_ = Delivery::unclaimed; // This boot's one-shot submit guard, not persisted.
};

} // namespace ayni::work
#endif
