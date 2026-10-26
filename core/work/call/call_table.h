// Logical call identities, retained business bodies and action ownership.
// No callbacks, mailbox slots, resource occupancy or wall-clock timestamps.
#ifndef AYNI_CORE_WORK_CALL_CALL_TABLE_H
#define AYNI_CORE_WORK_CALL_CALL_TABLE_H

#include "catalog/binding/binding_registry.h"
#include "work/action/action_record.h"
#include "work/call/call_input.h"
#include "work/call/tool_calls.h"
#include "work/execution/execution_fact.h"

namespace ayni::work {

// Immutable identity of a strategy step's accepted logical call. Exact binding
// and implementation selections survive restart; no latest-version fallback.
struct CallIdentity {
    uint64_t call_id;              // Stable Kernel ID, unique across every retained call.
    uint64_t operation_id;         // Distinct Kernel ID, allocated with call_id before preparation.
    uint64_t task_id;              // Owning Task identity, never its moving table index.
    uint64_t owner_instance_id;    // Task's immutable owner, used for resource fairness.
    catalog::CatalogId step_key;   // Unique within the Task, nonempty.
    catalog::CatalogId binding_id; // Exact trusted selection, nonempty.
    uint32_t binding_version;      // Positive selection version.
    catalog::CatalogId capability_id; // Exact authority/execution declaration.
    uint32_t capability_version;      // Positive semantic version.
    catalog::CatalogId provider_id;   // Exact implementation selected at acceptance.
    uint32_t provider_version;        // Positive implementation version.
    catalog::CatalogId endpoint;      // Trusted endpoint token, nonempty.
    catalog::CatalogId destination;   // Empty iff this contract has no EGRESS effect.
    // Value projection of the persisted selection; no pointers or mutable catalog index.
    catalog::Binding binding() const;
};

// Cancellation intent is independent of remote stop and terminal outcome.
enum class CallCancelCause : uint32_t { none = 0, user, authority, deadline, shutdown };

// One explicit owner-requested reconciliation. Its attempt never replaces the
// original execution attempt. Finished UNKNOWN may be queried again with a new command.
struct CallQuery {
    enum class Phase : uint32_t { requested = 1, dispatching, finished };
    uint32_t generation; // Starts at one; command CAS is generation + initial_query_revision.
    Phase phase; // Requested has no attempt; dispatching may have crossed its durable barrier.
    ActionStamp requested; // Original accepted management command, retained through completion.
    ayni_attempt_ref_v1 attempt{};  // Zero until dispatch preparation; immutable within generation.
    ayni_status_t status = AYNI_OK; // Query diagnostic, independent of the execution result.
    ayni_reason_t reason = AYNI_REASON_NONE; // Classified query failure; no provider error strings.
    uint32_t conclusion = 0;                 // Zero while pending; QUERY_RESULT_* when finished.
};
constexpr uint32_t initial_query_revision = 1; // No query has yet been requested.

// Facts a durable call owns in addition to its independently validated ledger.
struct CallState {
    ActionStamp created;            // Acceptance transition, before any possible submission.
    ActionStamp latest;             // Last state/body update, never an I/O timestamp.
    ayni_attempt_ref_v1 attempt{};  // Both zero before dispatch preparation; immutable once set.
    uint64_t progress_sequence = 0; // Greatest applied progress; zero before any progress.
    CallCancelCause cancel_cause = CallCancelCause::none; // First accepted cancellation intent.
    ayni_call_outcome_t cancel_result = 0; // None, CANNOT_CANCEL or CANCEL_UNKNOWN; not terminal.
    ayni_call_outcome_t outcome = 0; // Zero while pending; one terminal delivery outcome otherwise.
    ayni_status_t status = AYNI_OK;  // Classified terminal status; never vendor error text.
    ayni_reason_t reason = AYNI_REASON_NONE; // Terminal cause; no string data.
    std::optional<ExecutionFact>
        execution; // Physical constraints and independent stop; absent for ordinary I/O.
    std::optional<CallQuery> query; // Absent before owner reconciliation; never an execution retry.
};

// Borrowed terminal input already matched to the original attempt by the caller.
struct CallResult {
    ayni_call_outcome_t outcome; // COMPLETED, FAILED, CANCELLED or ABANDONED.
    ayni_status_t status;        // Public failure class, OK only for successful completion/cancel.
    ayni_reason_t reason;        // Public cause, NONE for success.
    ActionEffect effect;         // Explicit Provider knowledge, checked against outcome.
    ayni_string_v1 text;         // Copied bounded body as kind says; empty for ABANDONED.
    // UTF-8 text, or a tool-call list only in a TOOL_CALLS contract's completion.
    CallResultKind kind = CallResultKind::text;
};

// Whether exact input evidence still permits a result to advance dependent work.
enum class ResultDependencies { current, stale };

// Complete durable row. Byte views borrow the table or decoded record buffer;
// ActionFact is a value, never a copy of the volatile one-shot dispatch claim.
struct CallRecord {
    CallIdentity identity;            // Original step, owner and exact implementation selection.
    catalog::CallContract contract;   // Original normalized execution semantics.
    CallState state;                  // Latest business facts, no transport or resource handles.
    std::optional<ActionFact> action; // Absent until resource admission is recorded.
    ayni_bytes_v1 domain;             // Original authority schema bytes, required.
    ayni_bytes_v1 input;              // Canonical request authorized against domain, required.
    ayni_string_v1 result;            // Terminal body as result_kind says, empty before completion.
    CallResultKind result_kind;       // Text, unless a completion holds tool calls.
};

// Fixed slots keep noncopyable ActionRecord claims stable. Retained input and
// output bodies belong to this table until a safe Task retirement or deletion.
class CallTable {
  public:
    struct Limits {
        uint32_t capacity; // Zero disables calls; fixed entries, no eviction of unresolved work.
        uint32_t domain_bytes; // Positive original authority domain bytes per entry.
        uint32_t input_bytes;  // Positive canonical complete input bytes per entry.
        uint32_t result_bytes; // Positive UTF-8 bytes per entry; <= call_text_limit_max.
    };
    static constexpr uint32_t no_index = UINT32_MAX;                    // No retained call slot.
    static constexpr std::size_t storage_alignment = alignof(uint64_t); // Aligns metadata slots.
    // Includes metadata and three byte strides per slot; valid bounded limits required.
    static std::size_t storage_size(Limits limits);
    // Borrow aligned storage for this table lifetime; null only for zero capacity.
    CallTable(void *storage, Limits limits);
    CallTable(const CallTable &) = delete;
    CallTable &operator=(const CallTable &) = delete;
    // Global space check, no speculative eviction; owner retires safe completed Tasks.
    bool has_room() const { return size_ < limits_.capacity; }
    // Acceptance input/contract/domain have been validated together by admission.
    // Checks identity, uniqueness and arena sizes, then copies everything atomically.
    ayni_status_t insert(const CallIdentity &identity, const catalog::CallContract &contract,
                         ayni_bytes_v1 domain, ayni_bytes_v1 input, ActionStamp stamp,
                         uint32_t &index);
    // Attach prevalidated execution constraints in the same initial acceptance transition.
    void attach_execution(uint32_t index, const ExecutionFact &fact);
    // Independent local stop measurement, never inferred from Provider cancellation.
    ayni_status_t confirm_execution(uint32_t index, ayni_execution_phase_t phase,
                                    ayni_control_stop_cause_t cause, ayni_execution_target_t target,
                                    ActionStamp stamp);
    // Volatile I/O owner keeps exact call rows while still holding local execution references.
    void set_runtime_retention(const void *context, bool (*retains)(const void *, uint64_t));
    // Resource admission precedes action preparation; no Provider callback is possible here.
    ayni_status_t prepare(uint32_t index, ActionStamp stamp);
    // Persist the selected attempt and DISPATCHING barrier together, after preparation is durable.
    ayni_status_t begin_dispatch(uint32_t index, ayni_attempt_ref_v1 attempt, ActionStamp stamp,
                                 uint64_t durable_seq);
    // One volatile claim immediately before callback, following final owner checks.
    bool claim_dispatch(uint32_t index, uint64_t boot_epoch, uint64_t durable_seq);
    // Complete a call proven never handed to a Provider, including a queued call.
    ayni_status_t cancel_before_dispatch(uint32_t index, ActionStamp stamp);
    // Background wait expiration proves non-delivery and records EXPIRED/RESOURCE_WAIT.
    ayni_status_t skip_before_dispatch(uint32_t index, ActionStamp stamp);
    // Only the submit caller may assert NOT_RETAINED, after checking no receipt contradicted it.
    ayni_status_t rejected_not_retained(uint32_t index, ActionStamp stamp);
    // First request wins; caller rechecks Task/authority and submits cancellation separately.
    ayni_status_t request_cancel(uint32_t index, CallCancelCause cause, ActionStamp stamp);
    // Reliable control before terminal; does not finish work or release the attempt.
    ayni_status_t cancel_response(uint32_t index, ayni_call_outcome_t outcome, ActionStamp stamp);
    // Applied progress sequence must increase; no progress body is persisted as a final result.
    ayni_status_t progress(uint32_t index, uint64_t sequence, ActionStamp stamp);
    // Validate all scalar/body semantics before mutating action, call or result bytes.
    ayni_status_t finish(uint32_t index, const CallResult &result, ActionStamp stamp,
                         ResultDependencies dependencies = ResultDependencies::current);
    // Owner management CAS, independent of the revoked execution Grant.
    ayni_status_t check_query(uint32_t index, uint32_t expected_revision) const;
    uint32_t
    query_revision(uint32_t index) const; // Starts at one; advances once per accepted query.
    bool unresolved(
        uint32_t index) const; // A terminal outcome/effect still lacks authoritative knowledge.
    bool query_pending(uint32_t index) const; // Requested or dispatching, including after restart.
    ayni_status_t request_query(uint32_t index, ActionStamp stamp);
    ayni_status_t begin_query(uint32_t index, ayni_attempt_ref_v1 attempt, ActionStamp stamp);
    // UNKNOWN/transport failure preserves the original execution facts and Task hold.
    ayni_status_t end_query(uint32_t index, ayni_status_t status, ayni_reason_t reason,
                            ActionStamp stamp);
    // Exact operation evidence; best-effort absence cannot resolve an execution.
    ayni_status_t reconcile(uint32_t index, uint32_t conclusion, const CallResult &result,
                            ActionStamp stamp,
                            ResultDependencies dependencies = ResultDependencies::current);
    // Shared persisted query shape/history rules, including command generation changes.
    static bool valid_query(const CallRecord &record);
    static bool query_advances(const CallState &before, const CallState &after);
    // Task retirement prerequisite: every retained call is known and durable.
    bool can_retire_task(uint64_t task_id, uint64_t durable_seq) const;
    // Caller proved the Task terminal and can_retire_task true; wipes its bounded bodies.
    void retire_task(uint64_t task_id);
    // Clears replay-owned rows only after transport borrowers have been drained or isolated.
    void clear();
    // Export a row for record writing; views last until the next owner mutation.
    CallRecord record(uint32_t index) const;
    // Shared intrinsic validator for decoded records; cross-table proof belongs to Kernel.
    static bool valid_record(const CallRecord &record);
    // Replay-only: input passed valid_record; verify immutable history and capacity,
    // then install. Restored dispatch claims are sealed against repeat submission.
    ayni_status_t restore_validated(const CallRecord &record);
    // Stable ID and Task/step lookups; failures preserve index.
    bool find(uint64_t call_id, uint32_t &index) const;
    bool find_step(uint64_t task_id, ayni_string_v1 key, uint32_t &index) const;
    // Slots may contain holes; only occupied indices are legal for accessors/mutations.
    bool occupied(uint32_t index) const;
    uint32_t capacity() const { return limits_.capacity; } // Fixed metadata slots, including holes.
    uint32_t size() const { return size_; } // Retained calls, including unknown results.
    const CallIdentity &identity(uint32_t index) const;          // Borrowed immutable identity.
    const catalog::CallContract &contract(uint32_t index) const; // Saved original semantics.
    const CallState &state(uint32_t index) const;   // Borrowed until next owner mutation.
    const ActionFact *action(uint32_t index) const; // Null before resource admission/preparation.
    ayni_bytes_v1 domain(uint32_t index) const;     // Original schema, never a replacement catalog.
    ayni_bytes_v1 input(uint32_t index) const;      // Canonical bytes actually authorized and sent.
    ayni_string_v1 result(uint32_t index) const; // Retained terminal text, empty before terminal.
    // Retained tool-call list (tool_calls.h); empty unless the result holds tool calls.
    ayni_bytes_v1 tool_calls(uint32_t index) const;

  private:
    struct Entry {
        CallIdentity identity;          // Immutable acceptance identity, owns no views.
        catalog::CallContract contract; // Copied original semantics, not live registration state.
        CallState state;                // Sole owner of call/attempt/cancel/result facts.
        std::optional<ActionRecord> action; // Empty before resource admission; never copied.
        uint32_t domain_size;               // Used bytes in original schema stride.
        uint32_t input_size;                // Used bytes in canonical input stride.
        uint32_t result_size;       // Used UTF-8 result bytes, zero also permits empty success.
        CallResultKind result_kind; // How the result bytes read; text before a terminal.
    };
    struct Slot {
        std::optional<Entry> entry; // Empty means free; removing never moves another call.
    };
    // Admission shape excludes schema/authority, already checked by their owners.
    static bool valid_identity(const CallIdentity &identity, ActionStamp stamp);
    static bool advances(const Entry &entry, ActionStamp stamp); // Strict stamp order in this boot.
    ayni_status_t check_result(uint32_t index, const CallResult &result)
        const; // Original shape and UTF-8 before any dependency filtering.
    static CallResult
    usable_result(const CallResult &validated,
                  ResultDependencies dependencies); // Never normalizes invalid Provider evidence.
    static bool valid_result(const CallResult &result); // Shared execution/query result shape.
    // Body of either kind within this contract's result bound, before any mutation.
    static ayni_status_t check_body(const catalog::CallContract &contract, ayni_string_v1 body,
                                    CallResultKind kind);
    ayni_string_v1 body(uint32_t index) const; // Retained result bytes of either kind.
    static bool pending(const Entry &entry);   // No terminal call result has been committed.
    uint8_t *bytes(uint32_t index) const; // One address formula for the complete slot byte region.
    void set_terminal(Entry &entry, const CallResult &result, ActionStamp stamp);
    // Shared local non-delivery transition; callers supply a fixed validated Core outcome.
    ayni_status_t end_before_dispatch(uint32_t index, const CallResult &result, ActionStamp stamp);
    // Both admission and replay copy canonical byte bodies through one layout writer.
    void copy_bodies(uint32_t index, ayni_bytes_v1 domain, ayni_bytes_v1 input,
                     ayni_string_v1 result);

    Slot *slots_;     // Caller arena, stable objects for this table lifetime.
    uint8_t *bodies_; // Fixed domain/input/result regions, never heap allocation.
    Limits limits_;   // Immutable byte/count capacities, no resizing.
    const void *retention_context_ =
        nullptr; // Borrowed coordinator, absent in standalone table tests.
    bool (*runtime_retains_)(const void *, uint64_t) = nullptr; // Pure owner-thread query, no I/O.
    uint32_t size_ = 0; // Number of occupied optional entries, bounded by capacity.
};
} // namespace ayni::work
#endif
