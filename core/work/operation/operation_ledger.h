// Bodyless unresolved operations outlive retired Tasks (ADR-0115). Single owner,
// caller arena, no I/O or heap; active Calls and their private bodies stay in work/call.
#ifndef AYNI_CORE_WORK_OPERATION_OPERATION_LEDGER_H
#define AYNI_CORE_WORK_OPERATION_OPERATION_LEDGER_H
#include "work/call/call_table.h"

namespace ayni::work {
// Historical route only: no step, authority domain, input, result or live resource handles.
struct OperationIdentity {
    uint64_t call_id;              // Original logical Call ID, globally unique and never reused.
    uint64_t operation_id;         // Last subscription generation or ordinary stable operation ID.
    uint64_t task_id;              // Retired Task reference; never resolves to an active Task.
    uint64_t owner_instance_id;    // Retained AgentInstance; independent of execution Grants.
    catalog::CatalogId binding_id; // Exact original binding, nonempty.
    uint32_t binding_version;      // Positive original selection version.
    catalog::CatalogId capability_id; // Original capability semantics, nonempty.
    uint32_t capability_version;      // Positive original semantics version.
    catalog::CatalogId provider_id;   // Original implementation, nonempty.
    uint32_t provider_version;        // Positive original implementation version.
    catalog::CatalogId endpoint;      // Original endpoint token, nonempty; never a credential.
    catalog::CatalogId destination;   // Empty iff no EGRESS effect.
};
// Only independently confirmed stopped/not-started execution facts survive retirement.
struct ExecutionStopSummary {
    ayni_execution_phase_t phase;         // STOPPED or NOT_STARTED; never a live permit.
    ayni_control_stop_cause_t stop_cause; // NONE exactly when NOT_STARTED.
    ayni_execution_target_t target;       // UNVERIFIED when no physical verification exists.
};
// Durable scalar state. Revision advances only on a new owner query, preserving Call CAS.
struct OperationState {
    ActionStamp created;         // Original Call acceptance, before archive.
    ActionStamp archived;        // Atomic work replacement; strictly after original latest.
    ActionStamp latest;          // Archive or most recent query/closure, monotonically ordered.
    ayni_attempt_ref_v1 attempt; // Original ended execution attempt, both fields positive.
    ayni_call_outcome_t outcome; // Terminal tracking outcome, independent of unknown effects.
    ayni_status_t status;        // Classified original result, no vendor message.
    ayni_reason_t reason;        // Classified original cause, NONE when absent.
    std::optional<ExecutionStopSummary> execution_stop; // No summary for nonphysical Calls.
    std::optional<CallQuery> query; // Separate read-only reconciliation, never execution retry.
    uint32_t revision; // >=1; inherited from CallTable::query_revision before retirement.
};
// Codec value; views borrow the record buffer or ledger until the next mutation.
struct OperationRecord {
    OperationIdentity identity;     // Immutable retired operation and original route.
    catalog::CallContract contract; // Complete saved semantics and separately owned proof bytes.
    OperationState state;           // Durable stamps, outcome and explicit query lifecycle.
    ActionFact action;              // Exported value of the ledger-owned ActionRecord.
};
// One journal delta, including removals whose row no longer exists (records.md type30/v1).
struct OperationChange {
    enum class Kind : uint32_t {
        query_requested = 1,
        query_dispatching,
        query_failed,
        resolved,
        acknowledged
    };
    uint64_t call_id;    // Exact existing ledger row; never creates an operation.
    Kind kind;           // One of the five transitions above.
    uint32_t revision;   // Resulting owner CAS, unchanged except query_requested.
    uint32_t generation; // Current query generation; zero only for never-queried acknowledgement.
    ayni_call_outcome_t outcome; // Original or reconciled terminal outcome.
    ayni_status_t status;        // Query failure or closure status; never text.
    ayni_reason_t reason;        // Classified diagnostic; NONE when absent.
    ActionEffect effect;         // Original unknown or authoritative known effect.
    uint32_t conclusion;         // QUERY_RESULT_* or zero for acknowledged/requested/dispatching.
    ayni_attempt_ref_v1 attempt; // Query attempt; zero until preparation, never execution retry.
    ActionStamp stamp;           // This one transition, strictly after the row's latest stamp.
};
// RAM-only delivery obligation. Fenced reload discards it with uncommitted state.
struct PendingAudit {
    uint64_t call_id;      // Resolved logical Call; copied because its row is already removed.
    uint64_t operation_id; // Original operation identity.
    catalog::CatalogId binding_id; // Exact original route token.
    ayni_call_outcome_t outcome;   // Reconciled result or original acknowledged outcome.
    ayni_status_t status;          // Classified closure result.
    ayni_reason_t reason;          // Classified closure cause.
    bool acknowledged;  // True iff explicit human closure, never an authoritative result.
    bool physical;      // Saved contract declared a physical effect.
    uint64_t after_seq; // Emit only after this closure commit is durable.
};
class OperationLedger {
  public:
    struct Limits {
        uint32_t capacity = 0;       // Zero disables both table and audit queue, zero arena bytes.
        uint32_t business_bytes = 0; // Complete proof bytes per slot; matches Kernel capacity.
    };
    static constexpr std::size_t storage_alignment = alignof(uint64_t); // Caller arena alignment.
    static constexpr uint32_t no_index = UINT32_MAX;                    // No occupied slot.
    static std::size_t storage_size(Limits limits); // Metadata, proof strides and audit ring.
    OperationLedger(void *storage, Limits limits);  // Borrows arena until owner destruction.
    OperationLedger(const OperationLedger &) = delete;
    OperationLedger &operator=(const OperationLedger &) = delete;
    bool has_room(uint32_t count) const {
        return count <= limits_.capacity - size_;
    } // No eviction.
    uint32_t size() const { return size_; }                // Occupied unresolved rows.
    uint32_t capacity() const { return limits_.capacity; } // Fixed slots, including holes.
    bool occupied(uint32_t index) const;                   // False for out-of-range or empty slots.
    bool find(uint64_t call_id, uint32_t &index) const;    // Failure preserves index.
    bool contains_identity(uint64_t id) const; // Either call_id or operation_id, global namespace.
    OperationRecord record(uint32_t index) const; // Export original facts without private bodies.
    const OperationIdentity &identity(uint32_t index) const;     // Borrowed until owner mutation.
    const catalog::CallContract &contract(uint32_t index) const; // Complete immutable saved proof.
    const OperationState &state(uint32_t index) const;           // Borrowed scalar/query state.
    bool query_pending(uint32_t index) const;                    // Requested or dispatching.
    // Phase one checks every prerequisite without mutation; caller preflights the batch's count.
    ayni_status_t check_admit_from_call(const CallIdentity &, const catalog::CallContract &,
                                        const CallState &, const ActionFact &, ActionStamp) const;
    // Phase two cannot fail after all Calls pass preflight; copies the proof into its own slot.
    void admit_checked_call(const CallIdentity &, const catalog::CallContract &, const CallState &,
                            const ActionFact &, ActionStamp);
    ayni_status_t check_query(uint32_t index, uint32_t revision) const; // Owner CAS and contract.
    ayni_status_t request_query(uint32_t index,
                                ActionStamp stamp); // New query, increments revision.
    ayni_status_t begin_query(uint32_t index, ayni_attempt_ref_v1, ActionStamp); // Durable barrier.
    ayni_status_t end_query(uint32_t index, ayni_status_t, ayni_reason_t,
                            ActionStamp); // Retains row.
    // Scalar authoritative evidence only. Queue closure audit and immediately remove on success.
    ayni_status_t reconcile_and_remove(uint32_t index, uint32_t conclusion, const CallResult &,
                                       ActionStamp stamp);
    ayni_status_t check_acknowledge(uint32_t index,
                                    uint32_t revision) const; // CAS, no pending query.
    ayni_status_t acknowledge_and_remove(uint32_t index, ActionStamp stamp); // Human closure only.
    bool audit_queue_full() const {
        return audit_count_ == limits_.capacity;
    } // Closure backpressure.
    bool has_durable_audit(uint64_t durable_seq) const; // Ready after_seq at ring head.
    bool take_durable_audit(uint64_t durable_seq, PendingAudit &out); // Pop only confirmed closure.
    const std::optional<OperationChange> &journal_change() const {
        return journal_change_;
    } // Delta.
    static bool valid_record(const OperationRecord &record); // Shared codec/live intrinsic rules.
    ayni_status_t restore_snapshot_entry(const OperationRecord &record); // No audit from snapshots.
    ayni_status_t restore_journal_change(const OperationChange &change); // Reuses live transitions.
    void clear(); // Clears table, proof bytes, delta and pending audits on fenced reload.

  private:
    struct Entry {
        OperationIdentity identity;     // Sole owner of the historical route.
        catalog::CallContract contract; // business_contract borrows this slot's proof stride.
        OperationState state;           // Sole owner of scalar query and retirement facts.
        ActionRecord action; // Sole owner of side-effect knowledge; no dispatch path is exposed.
    };
    struct Slot {
        std::optional<Entry> entry;
    }; // Empty holes retain no identity or proof.
    uint8_t *proof(uint32_t index) const; // Independent fixed proof region for one slot.
    uint32_t empty_slot() const;          // Requires has_room(1), returns the first hole.
    void install(uint32_t index, const OperationRecord &record); // Already checked; copies proof.
    bool advances(uint32_t index,
                  ActionStamp stamp) const; // Strict boot/transition/sequence order.
    void changed(uint32_t index, OperationChange::Kind, ActionStamp); // One owner transition delta.
    void remove(uint32_t index, bool acknowledged,
                ActionStamp stamp); // Queues audit then wipes row.
    Slot *slots_;              // Fixed optional metadata in caller arena; null iff capacity zero.
    uint8_t *proofs_;          // No domain/input/result pools; null iff capacity zero.
    PendingAudit *audits_;     // Fixed ring, exactly capacity obligations; no heap.
    Limits limits_;            // Immutable arena bounds.
    uint32_t size_ = 0;        // Occupied slots, <= capacity.
    uint32_t audit_head_ = 0;  // Ring head, meaningful only with nonzero capacity.
    uint32_t audit_count_ = 0; // Pending closures; full causes BUSY before mutation.
    std::optional<OperationChange>
        journal_change_; // Last transition; stamp filters journal export.
};
} // namespace ayni::work
#endif
