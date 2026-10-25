// Kernel's bounded coordinator between work facts, resource admission and
// Provider delivery. No domain state is copied into this volatile I/O owner.
#ifndef AYNI_CORE_KERNEL_CALL_CALL_RUNTIME_H
#define AYNI_CORE_KERNEL_CALL_CALL_RUNTIME_H

#include "catalog/binding/binding_registry.h"
#include "kernel/completion/attempt_sequence.h"
#include "kernel/completion/provider_mailbox.h"
#include "kernel/intent/work_model.h"
#include "knowledge/observation/observation_table.h"
#include "resources/admission/resource_admission.h"
#include "resources/permit/permit_table.h"

namespace ayni::kernel {

// Borrowed immutable catalogs; all registration is closed before work starts.
struct CallCatalogs {
    const catalog::CallCapabilityRegistry &capabilities; // Execution semantics, not authority.
    const catalog::ProviderRegistry &providers; // Runtime-owned callback/context descriptors.
    const catalog::BindingRegistry &bindings;   // Exact selections, no automatic fallback.
    const catalog::ResourceRegistry &resources; // Resource IDs and capacity declarations.
};

// Owns only boot-local dispatch coordination, one staged decision and one copied
// receipt. Work/CallTable remains the sole owner of logical identities/results.
class CallRuntime {
  public:
    struct Limits {
        uint32_t capacity;     // Exactly CallTable's stable slot capacity; zero disables.
        uint32_t input_bytes;  // Staging stride, equal to the retained canonical input limit.
        uint32_t result_bytes; // Receipt stride, equal to the Provider mailbox payload bound.
    };
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(Limits limits); // Metadata, staging input and receipt bytes.
    // All owners and the aligned arena outlive this coordinator and its producers.
    CallRuntime(void *storage, Limits limits, WorkModel &work, CallCatalogs catalogs,
                resources::ResourceAdmission &resources, resources::PermitTable &permits,
                const knowledge::ObservationTable &observations, ProviderMailbox &mailbox,
                AttemptSequence &attempts, const ayni_host_v1 &host);
    CallRuntime(const CallRuntime &) = delete;
    CallRuntime &operator=(const CallRuntime &) = delete;

    // Install Task-scoped read/stage callbacks; end_decision seals them immediately
    // after decide returns, retaining only the copied proposal for validation.
    void begin_decision(uint32_t task_index, ayni_task_snapshot_v1 &snapshot,
                        ayni_policy_output_v1 &output);
    void end_decision();
    // One sole REQUEST intent must name the staged step and class. Checks all
    // capacities and authorization before the decision takes a transition stamp.
    authority::GrantCheck check_proposal(uint32_t task_index, const ayni_policy_intent_v1 &intent,
                                         uint64_t now_us, uint64_t durable_seq);
    // Preflight succeeded unchanged on the owner thread; accepts original identity,
    // canonical body, resource wait and Task state in this one transition.
    bool accept(uint32_t task_index, foundation::TransitionStamp stamp, uint64_t now_us);

    enum class Mode {
        running,
        draining,
        fenced,
        closing
    }; // Closing discards only unpublishable volatile receipts.
    enum class Advance {
        idle,
        applied,
        internal,
        attempt_exhausted
    }; // One bounded owner item, no waiting.
    Advance advance(uint64_t now_us, uint64_t durable_seq, Mode mode);
    // Independent local stop/lease ports run even when persistence cannot admit work.
    enum class JournalAdmission { available, blocked }; // Kernel's current commit capacity.
    Advance advance_managed(uint64_t now_us, uint64_t durable_seq, Mode mode,
                            JournalAdmission admission);
    Advance advance_control(uint64_t now_us, uint64_t durable_seq, Mode mode);
    bool control_ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const;
    // Ready/deadline projection does not invoke Providers or mutate domain state.
    bool ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const;
    // Pure projection of the selected call's pending dependency; never caches it in Task.
    ayni_call_wait_t waiting(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                             uint64_t &waited_us) const;
    uint64_t next_deadline_us() const; // Monotonic; UINT64_MAX when no timer is pending.
    bool idle() const; // No retained delivery/queued runtime, independent of durable history.
    // After an automatic storage reload, map retained transport handles back to
    // stable call IDs. Preserves pending receipts; does not restore submit claims.
    bool rebind_restored();

  private:
    enum class Stage { queued, prepared, barrier, submitted, waiting_stop, retiring, blocked };
    struct Delivery {
        ProviderReservation reservation; // Mailbox-owned exact incarnation, borrowed until ack.
        const catalog::Provider *provider =
            nullptr; // Borrowed stable port; survives model replay, null before submit.
        uint64_t deadline_us = UINT64_MAX; // None before actual callback; never persisted.
        enum class Cancel { none, sent } cancel = Cancel::none; // At most one bounded callback.
        enum class Purpose {
            execution,
            query
        } purpose = Purpose::execution; // Exact attempt protocol.
    };
    struct Active {
        uint64_t call_id; // Matches this CallTable slot; detects any premature replacement.
        Stage stage; // queued: no claim; prepared: claim only; barrier/submitted: delivery too.
        resources::ResourceClaim claim{}; // Empty while queued, otherwise exact held generation.
        std::optional<Delivery> delivery; // Present in barrier, submitted and waiting_stop stages.
    };
    struct Slot {
        std::optional<Active> active;
    }; // Empty has no boot-local ownership.
    struct Proposal {
        uint64_t task_id; // Task selected before callback; no mutable table index persists here.
        catalog::CatalogId step;     // Copied stable step identity, never a caller pointer.
        uint32_t binding_index;      // Immutable directory index, resolved during staging.
        catalog::BindingLinks links; // Catalog resolution, not cached authority or availability.
        uint32_t input_size;         // Canonical bytes in staging_, bounded by limits_.input_bytes.
        std::optional<work::ExecutionFact>
            execution;                  // Validated original constraints for physical calls only.
        ayni_policy_intent_kind_t kind; // REQUEST_CALL or REQUEST_MODEL, matched at consumption.
    };
    struct Decision {
        uint32_t task_index; // Stable during the synchronous callback only.
        ayni_policy_intent_v1
            *intents; // Original Kernel buffer, never trust a replaced output pointer.
        ayni_policy_output_v1 *output; // Kernel's actual stack output, not a replacement pointer.
    };
    static ayni_status_t stage_request(void *context, const ayni_call_request_v1 *request);
    static ayni_status_t read_result(const void *context, ayni_string_v1 step,
                                     ayni_task_call_result_v1 *out);
    ayni_status_t stage(const ayni_call_request_v1 &request);
    authority::GrantCheck check_input(uint32_t task_index, const catalog::Binding &binding,
                                      ayni_bytes_v1 domain, const work::CallInputView &input,
                                      uint64_t now_us, uint64_t durable_seq) const;
    resources::ResourceRequest resource_request(uint64_t call_id, uint64_t owner,
                                                const catalog::BindingLinks &links,
                                                const catalog::CallContract &contract,
                                                const catalog::WorkProfile &profile) const;
    // Fencing never drops pending receipts. Explicit unwritable shutdown may
    // discard volatile outcomes without reporting a committed business success.
    Advance drain_transport(Mode mode);
    Advance
    cancel_transport(Delivery &delivery); // One shared cancel port contract, never a terminal.
    bool transport_ready(Mode mode) const;
    Advance consume(uint64_t now_us);
    // Applies one exact receipt to its invariant owner, before mailbox acknowledgement.
    ayni_status_t apply_receipt(uint32_t index, uint32_t task_index, const ProviderMessage &message,
                                foundation::TransitionStamp stamp, bool &terminal);
    Advance advance_query(uint32_t index, uint64_t now_us, uint64_t durable_seq, Mode mode);
    Advance prepare_query(uint32_t index);
    Advance dispatch_query(uint32_t index, uint64_t now_us);
    bool query_ready(uint32_t index, uint64_t now_us, uint64_t durable_seq, Mode mode) const;
    bool consume_query(uint32_t index, uint32_t task_index, const ProviderMessage &message,
                       foundation::TransitionStamp stamp);
    Advance end_query(uint32_t index, ayni_status_t status, ayni_reason_t reason);
    Advance advance_call(uint32_t index, uint64_t now_us, uint64_t durable_seq, Mode mode);
    Advance restore_wait(uint32_t index, uint32_t task_index, uint64_t now_us, Mode mode);
    Advance progress_dispatch(uint32_t index, uint32_t task_index, uint64_t now_us,
                              uint64_t durable_seq);
    Advance stop(uint32_t index, uint32_t task_index, Mode mode, work::CallCancelCause cause);
    Advance dispatch(uint32_t index, uint32_t task_index, uint64_t now_us, uint64_t durable_seq);
    Advance expire_wait(uint32_t index, uint32_t task_index, resources::WaitAction action,
                        uint64_t now_us);
    Advance cancel_local(uint32_t index, uint32_t task_index);
    // Result, step and lifecycle share one B4 stamp; cancellation never reopens work.
    bool finish_task(uint32_t call_index, uint32_t task_index, foundation::TransitionStamp stamp);
    void release(uint32_t index, uint64_t now_us); // Exact claim/waiter; delivery already drained.

    authority::GrantCheck prepare_execution(Proposal &proposal, ayni_bytes_v1 arguments,
                                            uint64_t now_us, uint64_t durable_seq) const;
    work::ResultDependencies result_dependencies(
        uint32_t index) const; // Exact evidence state shared by original and query results.
    bool execution_dependencies_current(
        uint32_t index, uint64_t now_us) const; // Shared dispatch/result prerequisite check.
    bool execution_allowed(uint32_t index, uint64_t now_us, uint64_t durable_seq) const;
    ayni_status_t issue_permit(uint32_t index, ayni_bytes_v1 arguments, uint64_t now_us);
    Advance advance_urgent_stop(uint64_t now_us, uint64_t durable_seq, Mode mode);
    Advance poll_executor(uint32_t index, uint64_t now_us, uint64_t durable_seq, Mode mode);
    Advance commit_stop(uint32_t index);
    bool active_index(uint64_t call_id,
                      uint32_t &index) const; // Stable identity lookup survives work-table discard.
    bool delivery_expired(uint32_t index, uint64_t now_us)
        const; // One timeout predicate for scheduling and independent stop.
    bool should_stop(const resources::PermitLease &lease, uint64_t now_us, uint64_t durable_seq,
                     Mode mode) const; // One shared readiness/renewal gate.
    bool stopped_releasable(const resources::PermitLease &lease, uint64_t durable_seq, Mode mode)
        const; // Durable proof, or explicit unwritable shutdown after drain.
    Advance renew_permit(resources::PermitLease &lease);
    Advance submit_provider(uint32_t index, uint32_t task_index, uint32_t provider_index,
                            const work::CallInputView &input, uint64_t now_us);
    ProviderMailbox::BlockedSlots
    blocked_receipts() const; // Only retained terminals awaiting physical proof.
    static bool runtime_retains(const void *context, uint64_t call_id);
    static bool submitted(Stage stage) {
        return stage == Stage::submitted || stage == Stage::waiting_stop;
    }

    WorkModel &work_;       // Domain facts, never accessed by Provider worker threads.
    CallCatalogs catalogs_; // Borrowed immutable selection contracts.
    resources::ResourceAdmission &resources_; // Sole owner of occupancy and queue fairness.
    resources::PermitTable
        &permits_; // Resource-owned local leases, no domain facts copied into Kernel.
    const knowledge::ObservationTable
        &observations_;         // Exact dependency validity remains knowledge-owned.
    ProviderMailbox &mailbox_;  // Sole owner of concurrent copied deliveries.
    AttemptSequence &attempts_; // Shared Host/Provider attempt identity allocator.
    const ayni_host_v1
        &host_;        // Bounded callback clock/lock/wake services, lifetime held by Kernel.
    Limits limits_;    // Immutable arena dimensions, shared bounds with CallTable/mailbox.
    Slot *slots_;      // One optional volatile owner per stable call slot.
    uint8_t *staging_; // Canonical proposed input, copied while callback views are alive.
    uint8_t *receipt_; // Owner copy; mailbox retains original until domain publication succeeds.
    std::optional<Decision> decision_; // Present only during decide; callback capability lease.
    std::optional<Proposal> proposal_; // Copied proposal survives callback until consumed/reset.
    enum class NextLane {
        control,
        work
    } next_lane_ = NextLane::control; // Routine polling yields every other turn to business work.
    uint32_t control_cursor_ = 0;     // Separate fair scan for independent executor ports.
    uint32_t cursor_ = 0;             // Fair bounded scan of active calls, zero when disabled.
};
} // namespace ayni::kernel
#endif
