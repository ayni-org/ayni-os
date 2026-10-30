// Kernel's bounded coordinator between work facts, resource admission and
// Provider delivery. No domain state is copied into this volatile I/O owner.
#ifndef AYNI_CORE_KERNEL_CALL_CALL_RUNTIME_H
#define AYNI_CORE_KERNEL_CALL_CALL_RUNTIME_H

#include "kernel/config/catalogs.h"
#include "kernel/ports/attempt_sequence.h"
#include "kernel/ports/provider_mailbox.h"
#include "kernel/ports/subscription_ingress_port.h"
#include "kernel/service/execution/execution_control.h"
#include "kernel/service/model/work_model.h"
#include "knowledge/observation/observation_table.h"
#include "resources/admission/resource_admission.h"
#include "resources/permit/permit_table.h"

namespace ayni::kernel {

// Owns only boot-local dispatch coordination, one staged decision and one copied
// receipt. Work/CallTable remains the sole owner of logical identities/results;
// physical execution is ExecutionControl's, asked at each gate.
//
// A now_us argument is the step's start, for scheduling, queue order and
// delivery deadlines. The Grant check right before dispatch, a result's
// observation dependencies and a release's queue time read host_'s clock
// again, so time spent in the step's earlier callouts counts (ADR-0051).
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
    CallRuntime(void *storage, Limits limits, WorkModel &work, const Catalogs &catalogs,
                resources::ResourceAdmission &resources, resources::PermitTable &permits,
                knowledge::ObservationTable &observations,
                SubscriptionIngressPort subscription_ingress, ProviderMailbox &mailbox,
                ExecutorMailbox &executors, AttemptSequence &attempts, const ayni_host_v1 &host);
    CallRuntime(const CallRuntime &) = delete;
    CallRuntime &operator=(const CallRuntime &) = delete;

    // Install Task-scoped read/stage callbacks; end_decision seals them immediately
    // after decide returns, retaining only the copied proposal for validation.
    void begin_decision(uint32_t task_index, ayni_task_snapshot_v1 &snapshot,
                        ayni_policy_output_v1 &output, uint64_t durable_seq);
    void end_decision();
    // After publication/refusal, erase the copied proposal text, including unused buffer tail.
    void scrub_decision();
    // One sole REQUEST intent must name the staged step and class. Checks all
    // capacities and authorization before the decision takes a transition stamp.
    authority::GrantCheck check_proposal(uint32_t task_index, const ayni_policy_intent_v1 &intent,
                                         uint64_t now_us, uint64_t durable_seq,
                                         knowledge::MemoryDependencies &dependencies);
    // Preflight succeeded unchanged on the owner thread; accepts original identity,
    // canonical body, resource wait and Task state in this one transition.
    bool proposal_needs_redecision() const { return proposal_ && proposal_->redecide; }
    bool accept(uint32_t task_index, foundation::TransitionStamp stamp, uint64_t now_us);

    // Task-scoped borrowed directory; no Provider callback or authority mutation.
    ayni_status_t list_capabilities(uint32_t task_index, uint64_t now_us, uint64_t durable_seq,
                                    const ayni_capability_cursor_v1 &cursor,
                                    ayni_capability_info_v1 *items, uint32_t capacity,
                                    uint32_t &count, ayni_capability_cursor_v1 &next) const;
    ayni_status_t inspect_capability(uint32_t task_index, uint64_t now_us, uint64_t durable_seq,
                                     ayni_string_v1 binding_id, uint32_t version,
                                     ayni_capability_detail_v1 &out) const;
    using Mode = RuntimeMode;
    using Advance = RuntimeAdvance;
    Advance advance(uint64_t now_us, uint64_t durable_seq, Mode mode);
    // Independent local stop/lease ports run even when persistence cannot admit work.
    enum class JournalAdmission { available, blocked }; // Kernel's current commit capacity.
    Advance advance_managed(uint64_t now_us, uint64_t durable_seq, Mode mode,
                            JournalAdmission admission);
    bool control_ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const {
        return subscription_expiry_ready(now_us) || subscription_cancel_ready() ||
               control_.ready(now_us, durable_seq, mode);
    }
    // Ready/deadline projection does not invoke Providers or mutate domain state.
    bool ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const;
    // Source/Task refusal frontier closes all affected gates even with no journal room.
    // This performs bounded owner bookkeeping only, never Provider callbacks.
    // Capture transient management gates before a following resume; no Provider/Executor I/O.
    // False is a resource-owner invariant failure, never a completed stop.
    bool refresh_live_gates(uint64_t now_us, uint64_t durable_seq, Mode mode);
    // Task and current lease must still permit collection, independent of queued transport.
    bool subscription_usable(uint32_t index, uint64_t now_us, uint64_t durable_seq) const;
    // Trusted owner projection; phase is distinct from the underlying Call action outcome.
    ayni_status_t subscription_info(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                    ayni_subscription_info_v1 &out) const;
    // Pure projection of the selected call's pending dependency; never caches it in Task.
    ayni_call_wait_t waiting(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                             uint64_t &waited_us) const;
    uint64_t next_deadline_us() const; // Monotonic; UINT64_MAX when no timer is pending.
    bool idle() const; // No retained delivery/queued runtime, independent of durable history.
    // Every erased call has released its local transport/Provider borrow; unrelated calls may run.
    bool privacy_released() const;
    // After an automatic storage reload, map retained transport handles back to
    // stable call IDs. Preserves pending receipts; does not restore submit claims.
    bool rebind_restored();

  private:
    enum class Stage {
        queued,
        prepared,
        barrier,
        submitted,
        waiting_stop,
        retiring,
        lease_stop,
        blocked
    };
    // A requested absolute deadline is kept until its exact reliable ACK arrives.
    struct LeaseRequest {
        uint64_t sequence;           // Positive control serial, STARTED is 1.
        uint64_t requested_until_us; // Exact finite extension proposed to Provider.
        uint64_t renew_at_us;        // Half-duration schedule fixed when the request was issued.
    };
    struct CollectionLease {
        uint64_t
            valid_until_us;   // Confirmed/current opening deadline, never extended by ACK arrival.
        uint64_t renew_at_us; // Half-duration point; UINT64_MAX while awaiting control or closing.
        std::optional<work::CallCancelCause>
            inhibition{};                    // Sticky refusal before its journal row fits.
        std::optional<LeaseRequest> pending; // None when no requested ACK is outstanding.
    };
    // Saved positions from one immutable Goal policy. Membership/count share one owner;
    // no machine-word mask limits a directory to 8/16/32 business tools.
    struct ToolSelection {
        std::array<uint8_t, AYNI_GOAL_TYPE_TOOL_COUNT_MAX> positions{}; // Zero/one membership.
        uint32_t count = 0; // Number of included positions, zero is explicitly empty.
        // Include one valid frozen position once, keeping count equal to membership.
        void include(uint32_t position) {
            if (!positions[position]) {
                positions[position] = 1;
                ++count;
            }
        }
        // Whether this valid frozen position belongs to the saved selection.
        bool contains(uint32_t position) const { return positions[position] != 0; }
        // Index-th included position; false preserves out when there are fewer.
        bool nth(uint32_t index, uint32_t &out) const {
            for (uint32_t position = 0; position < positions.size(); ++position) {
                if (!contains(position)) {
                    continue;
                }
                if (!index) {
                    out = position;
                    return true;
                }
                --index;
            }
            return false;
        }
    };
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
        ToolSelection tools{};          // Exact Goal policy positions offered at submit.
        std::optional<work::CallCancelCause> inhibition{}; // Sticky user stop for this execution
                                                           // attempt until budgeted cancellation.
    };
    struct Active {
        uint64_t call_id; // Matches this CallTable slot; detects any premature replacement.
        Stage stage; // queued: no claim; prepared: claim only; barrier/submitted: delivery too.
        resources::ResourceClaim claim{}; // Empty while queued, otherwise exact held generation.
        std::optional<Delivery>
            delivery; // Present in barrier/submitted/waiting_stop, never lease_stop.
        std::optional<CollectionLease>
            collection{}; // Subscription lease outlives an uncertain ended borrow.
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
        bool redecide = false; // Final optional admission expired: discard this decision only.
        work::AuthorityReference
            authorization{}; // Optional tool's whole initial Grant, zero for required.
        std::optional<work::ExecutionFact>
            execution; // Validated original constraints for physical calls only.
        std::optional<uint32_t> subscription_index; // Present only for Task-local close or wait.
        std::optional<work::SubscriptionWait>
            wait_basis;                 // Present iff this proposal waits at a copied frontier.
        ayni_policy_intent_kind_t kind; // REQUEST_CALL or REQUEST_MODEL, matched at consumption.
    };
    struct Decision {
        uint32_t task_index;  // Stable during the synchronous callback only.
        uint64_t durable_seq; // Acknowledged prefix shared with the memory decision lease.
        uint64_t now_us; // Decision clock sample; final admission checks again before publication.
        ayni_policy_intent_v1
            *intents; // Original Kernel buffer, never trust a replaced output pointer.
        ayni_policy_output_v1 *output; // Kernel's actual stack output, not a replacement pointer.
    };
    // One GoalType tool as a Task's model call may be offered it (ADR-0054).
    struct ToolOffer {
        const catalog::Binding *binding; // Immutable registry entry.
        catalog::BindingLinks links;     // Its capability and call contract.
        ayni_bytes_v1 domain;            // The capability's registered domain.
        ayni_bytes_v1 ceiling;           // The Goal requirement scope that admits its route.
    };
    // The submit in progress, for the Provider's tool callback only.
    struct Offering {
        uint32_t task_index;     // Stable during the synchronous submit.
        ToolSelection positions; // Frozen positions in the owning Goal policy.
    };
    // Directory callback leases close with decide, like retained Call reads.
    static ayni_status_t list_capabilities_view(const void *context,
                                                const ayni_capability_cursor_v1 *cursor,
                                                ayni_capability_info_v1 *items, uint32_t capacity,
                                                uint32_t *count, ayni_capability_cursor_v1 *next);
    static ayni_status_t inspect_capability_view(const void *context, ayni_string_v1 binding_id,
                                                 uint32_t version, ayni_capability_detail_v1 *out);
    bool visible_capability(uint32_t task_index, uint32_t position, uint64_t now_us,
                            uint64_t durable_seq, uint32_t &binding_index,
                            catalog::BindingLinks &links) const;
    static ayni_status_t stage_request(void *context, const ayni_call_request_v1 *request);
    // Resolve only within the active Task decision and current content capability.
    ayni_status_t subscription_step(ayni_string_v1 step, uint32_t &index) const;
    // Task-scoped copies, only while the strategy's decision capability is active.
    static ayni_status_t read_subscription(const void *context, ayni_string_v1 step,
                                           ayni_subscription_info_v1 *out);
    static ayni_status_t read_subscription_observation(const void *context, ayni_string_v1 step,
                                                       ayni_observation_info_v1 *out);
    static ayni_status_t read_result(const void *context, ayni_string_v1 step,
                                     ayni_task_call_result_v1 *out);
    static ayni_status_t read_tool_call(const void *context, ayni_string_v1 step, uint32_t index,
                                        ayni_task_tool_call_v1 *out);
    // Listed tool at position, if this Task's immutable Goal ceiling covers
    // its capability and admits the Binding's route; no Grant liveness check.
    bool offer_tool(uint32_t task_index, uint32_t position, ToolOffer &out) const;
    ToolSelection offered_tools(uint32_t task_index) const; // Finite frozen policy positions.
    static ayni_status_t describe_tool(const void *context, uint32_t index, ayni_tool_v1 *out);
    // Every call names an offered tool with values in its ceiling and route and
    // text within its limit. Shape was checked when posted; this adds the catalogs.
    ayni_status_t normalize_tool_calls(uint32_t task_index, const ToolSelection &positions,
                                       ayni_bytes_v1 list, uint32_t &size);
    // One selected tool's original Schema resolves native or lossless adapter input.
    ayni_status_t normalize_tool_input(uint32_t task_index, const ToolOffer &offer,
                                       const work::ToolCallView &call,
                                       catalog::DecodedValue &out) const;
    // Normalize a copied structured terminal with this Call's original result schema.
    ayni_status_t normalize_result(uint32_t index, ayni_bytes_v1 body, work::CallResult &result);
    ayni_status_t stage(const ayni_call_request_v1 &request);
    // Close uses only a Task-local step; it never reauthorizes capture or allocates I/O.
    ayni_status_t stage_subscription_target(const ayni_call_request_v1 &request);
    // Install one copied proposal and its matching policy intent in the original output.
    void stage_intent(const Proposal &proposal);
    authority::GrantCheck check_input(uint32_t task_index, const catalog::Binding &binding,
                                      ayni_bytes_v1 domain, const work::CallInputView &input,
                                      const catalog::CallContract &contract, uint64_t now_us,
                                      uint64_t durable_seq, uint64_t source_call_id = 0,
                                      work::AuthorityReference *selected = nullptr) const;
    // Optional directory membership does not replace an existing required ceiling.
    bool on_demand_tool(uint32_t task_index, const catalog::Binding &binding,
                        const catalog::CallContract &contract) const;
    // Typed values require one original requirement to cover every projected row.
    authority::GrantCheck check_business_proof(uint32_t task_index, const catalog::Binding &binding,
                                               ayni_bytes_v1 domain, const ayni_value_v1 &value,
                                               const catalog::CallContract &contract,
                                               const ayni_business_contract_v1 &proof) const;
    resources::ResourceRequest resource_request(uint64_t call_id, uint64_t owner,
                                                const catalog::BindingLinks &links,
                                                const catalog::CallContract &contract,
                                                const catalog::WorkProfile &profile) const;
    // Fencing never drops pending receipts. Explicit unwritable shutdown may
    // discard volatile outcomes without reporting a committed business success.
    Advance drain_transport(Mode mode);
    static constexpr uint64_t microseconds_per_millisecond = 1000; // Host request unit conversion.
    // Current latest sample/state frontier, independent of renewal ACK count.
    work::SubscriptionWait subscription_wait_basis(uint32_t index) const;
    // A waiting Task wakes once when its latest frontier changes; no polling decision.
    bool next_subscription_wakeup(uint64_t now_us, uint64_t durable_seq,
                                  uint32_t &task_index) const;
    // One lease computation serves initial submit and every renewal.
    static LeaseRequest plan_subscription_lease(uint64_t now_us, uint32_t lease_ms,
                                                uint64_t sequence);
    // Bind closed sink and cache lifetime before the first Provider callback.
    bool prepare_subscription_delivery(uint32_t index, uint64_t now_us,
                                       ayni_subscription_open_v1 &out);
    // Reliable lease ACK is not a terminal and must not release the transport reservation.
    Advance consume_subscription_control(uint32_t index, uint32_t task_index,
                                         const ProviderMessage &message,
                                         const ProviderReceipt &receipt, uint64_t now_us,
                                         uint64_t durable_seq);
    // One renewal request, reusing complete saved arguments and fresh authority checks.
    Advance renew_subscription(uint32_t index, uint64_t now_us, uint64_t durable_seq);
    // Independent cancellations stay ready even while journal admission is blocked.
    bool subscription_cancel_ready() const;
    // Pending domain stop remains ready after its independent cancel was already sent.
    bool subscription_stop_pending(uint32_t index) const;
    // Uncertain collection retains its units until the longest requested lease ends.
    static uint64_t collection_stop_deadline(const CollectionLease &lease);
    void await_subscription_stop(uint32_t index);
    bool subscription_expiry_ready(uint64_t now_us) const;
    Advance release_expired_subscriptions(uint64_t now_us);
    // Routine renew readiness is reported only through the journal-admitted work lane.
    bool subscription_renew_ready(uint32_t index, uint64_t now_us, uint64_t durable_seq) const;
    // Revoke the locked producer projection and cached knowledge in the same owner turn.
    void close_subscription_gate(uint32_t index);
    // One independent cancel/renew callback within the step budget; no wait for persistence.
    Advance advance_subscription_control(uint64_t now_us, uint64_t durable_seq, Mode mode,
                                         JournalAdmission admission);
    // Persist this generation's previously established refusal before consuming a terminal.
    Advance commit_subscription_stop(uint32_t index);
    // A stopped preserved subscription can rejoin resource admission, with a new operation.
    Advance reopen_subscription(uint32_t index, uint32_t task_index, uint64_t now_us,
                                uint64_t durable_seq, Mode mode);
    // Same eligibility predicate for advance and readiness; no eager new-generation allocation.
    bool subscription_reopen_ready(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                   Mode mode) const;
    // One original-selection/domain compatibility proof for restore and normal subscription reopen.
    bool compatible_call(uint32_t index, catalog::BindingLinks &links) const;
    Advance
    cancel_transport(Delivery &delivery); // One shared cancel port contract, never a terminal.
    bool transport_ready(Mode mode) const;
    Advance consume(uint64_t now_us, uint64_t durable_seq);
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
    // Scan domain calls and their shared admission queue after reliable ingress.
    bool calls_ready(uint64_t now_us, uint64_t durable_seq, Mode mode) const;
    Advance advance_call(uint32_t index, uint64_t now_us, uint64_t durable_seq, Mode mode);
    Advance restore_wait(uint32_t index, uint32_t task_index, uint64_t now_us, uint64_t durable_seq,
                         Mode mode);
    Advance progress_dispatch(uint32_t index, uint32_t task_index, uint64_t now_us,
                              uint64_t durable_seq);
    Advance stop(uint32_t index, uint32_t task_index, Mode mode, work::CallCancelCause cause);
    Advance dispatch(uint32_t index, uint32_t task_index, uint64_t now_us, uint64_t durable_seq);
    Advance expire_wait(uint32_t index, uint32_t task_index, resources::WaitAction action,
                        uint64_t now_us);
    Advance cancel_local(uint32_t index, uint32_t task_index);
    void release(uint32_t index, uint64_t now_us); // Exact claim/waiter; delivery already drained.

    bool active_index(uint64_t call_id,
                      uint32_t &index) const; // Stable identity lookup survives work-table discard.
    bool delivery_expired(uint32_t index, uint64_t now_us)
        const; // One timeout predicate for scheduling and independent stop.
    // The slot of a call whose stop fact must become durable before release.
    bool retiring_index(uint64_t call_id, uint32_t &index) const;
    // ExecutionControl::Transport over the slots below; context is the runtime.
    static bool transport_holds(const void *context, uint64_t call_id, uint64_t now_us,
                                bool &out_expired);
    static bool transport_retiring(const void *context, uint64_t call_id);
    static void transport_retire(void *context, uint64_t call_id, uint64_t now_us);
    Advance submit_provider(uint32_t index, uint32_t task_index, uint32_t provider_index,
                            const work::CallInputView &input, uint64_t now_us);
    ProviderMailbox::BlockedSlots
    blocked_receipts() const; // Only retained terminals awaiting physical proof.
    static bool runtime_retains(const void *context, uint64_t call_id);
    static bool submitted(Stage stage) {
        return stage == Stage::submitted || stage == Stage::waiting_stop;
    }

    knowledge::ObservationTable
        &observations_; // Borrowed knowledge owner, never copied sample state.
    SubscriptionIngressPort
        subscription_ingress_; // Fixed capability for locked worker-gate projection.
    WorkModel &work_;          // Domain facts, never accessed by Provider worker threads.
    const Catalogs &catalogs_; // Closed before work starts.
    resources::ResourceAdmission &resources_; // Sole owner of occupancy and queue fairness.
    ProviderMailbox &mailbox_;                // Sole owner of concurrent copied deliveries.
    AttemptSequence &attempts_;               // Shared Host/Provider attempt identity allocator.
    const ayni_host_v1
        &host_;           // Bounded callback clock/lock/wake services, lifetime held by Kernel.
    Limits limits_;       // Immutable arena dimensions, shared bounds with CallTable/mailbox.
    Slot *slots_;         // One optional volatile owner per stable call slot.
    uint8_t *staging_;    // Canonical proposed input, copied while callback views are alive.
    uint8_t *receipt_;    // Owner copy; mailbox retains original until domain publication succeeds.
    uint8_t *normalized_; // Owner-only result normalization bytes, erased after consumption.
    std::optional<Decision> decision_; // Present only during decide; callback capability lease.
    std::optional<Proposal> proposal_; // Copied proposal survives callback until consumed/reset.
    std::optional<Offering> offering_; // Present only while a Provider's submit runs.
    enum class NextLane {
        control,
        work
    } next_lane_ = NextLane::control; // Routine polling yields every other turn to business work.
    uint32_t cursor_ = 0;             // Fair bounded scan of active calls, zero when disabled.
    ExecutionControl control_;        // Permits, leases and measured stops of physical calls.
};
} // namespace ayni::kernel
#endif
