// Representation of ayni_kernel, shared by the kernel sources and white-box
// unit tests. Nothing outside core/kernel and tests/ may include it.

#ifndef AYNI_CORE_KERNEL_KERNEL_STATE_H
#define AYNI_CORE_KERNEL_KERNEL_STATE_H

#include "catalog/capability/workspace.h"
#include "kernel/config/catalogs.h"
#include "kernel/config/kernel_config.h"
#include "kernel/ports/completion_pool.h"
#include "kernel/ports/executor_mailbox.h"
#include "kernel/ports/provider_mailbox.h"
#include "kernel/runtime/boot/boot_sequence.h"
#include "kernel/runtime/commit/commit_log.h"
#include "kernel/runtime/ingress/event_ingress.h"
#include "kernel/runtime/ingress/observation_ingress.h"
#include "kernel/runtime/ingress/observation_slots.h"
#include "kernel/runtime/kernel_layout.h"
#include "kernel/runtime/step_context.h"
#include "kernel/runtime/storage/storage_budget.h"
#include "kernel/runtime/storage/storage_maintenance.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/decision/decision.h"
#include "kernel/service/intent/intent.h"
#include "kernel/service/model/work_model.h"
#include "kernel/service/persistence/work_journal.h"
#include "kernel/service/persistence/work_replay.h"
#include "kernel/service/reflex/reflex_engine.h"
#include "kernel/service/reflex/reflex_table.h"
#include "knowledge/observation/observation_table.h"
#include "resources/admission/resource_admission.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

class KernelTestProbe; // The sole white-box test accessor; never part of the ABI.

// How one owner item ended (ayni_kernel::run_transition): nothing to do, one
// transition applied, or a defect that restricts the kernel.
struct ItemEnd {
    enum class Kind { none, applied, defect } kind;
    ayni_restriction_t restriction = AYNI_RESTRICTION_NONE; // A defect's.
};

} // namespace ayni::kernel

// Lives at the start of the caller's memory block; the stores follow it at
// the offsets of kernel_layout.h.
struct ayni_kernel {
    ayni_kernel(const ayni_host_v1 &host_table, const ayni::kernel::KernelCapacities &capacities,
                const ayni::kernel::KernelLimits &limits,
                const ayni::kernel::KernelStorage &storage)
        : workspace(storage[ayni::kernel::Store::workspace], ayni::kernel::owner_workspace_size()),
          host(host_table),
          ingress(storage[ayni::kernel::Store::ingress], capacities.ingress_capacity,
                  capacities.event_body_max_bytes, host),
          completions(storage[ayni::kernel::Store::completions], capacities.completion_slot_count,
                      host, attempts),
          writes(limits.storage_write_interval_us), catalogs(capacities, storage, workspace),
          work(catalogs, capacities, limits, storage, callbacks, workspace), replay(work),
          boot(host, completions, storage.bytes(ayni::kernel::Store::storage_buffer),
               capacities.storage_buffer_bytes, work, replay, writes),
          log(host, completions, storage.bytes(ayni::kernel::Store::storage_buffer),
              capacities.storage_buffer_bytes, writes),
          maintenance(host, completions, storage.bytes(ayni::kernel::Store::storage_buffer),
                      capacities.storage_buffer_bytes, work, writes),
          decisions(storage[ayni::kernel::Store::decisions],
                    ayni::kernel::decision_limits(capacities)),
          budget(capacities, limits),
          resources(catalogs.resources, storage[ayni::kernel::Store::resource_states],
                    capacities.resource_capacity, storage[ayni::kernel::Store::resource_requests],
                    capacities.resource_request_capacity),
          provider_completions(storage[ayni::kernel::Store::provider_completions],
                               ayni::kernel::provider_mailbox_limits(capacities), host),
          permits(storage[ayni::kernel::Store::permits], capacities.executor_capacity, resources),
          executor_completions(storage[ayni::kernel::Store::executor_completions],
                               capacities.executor_capacity, host),
          observations(catalogs.observation_sources, storage[ayni::kernel::Store::observations],
                       capacities.observation_source_capacity, capacities.call_capacity),
          observation_ingress(storage[ayni::kernel::Store::observation_ingress],
                              capacities.observation_ingress_capacity,
                              capacities.observation_source_capacity, catalogs.observation_sources,
                              host, capacities.call_capacity),
          observation_slots(storage[ayni::kernel::Store::observation_slots],
                            capacities.observation_source_capacity, catalogs.observation_sources),
          calls(storage[ayni::kernel::Store::call_runtime],
                ayni::kernel::call_runtime_limits(capacities), work, catalogs, resources, permits,
                observations, observation_ingress.subscription_port(), provider_completions,
                executor_completions, attempts, host),
          reflex_table(storage[ayni::kernel::Store::reflex_rules], capacities.reflex_capacity,
                       capacities.call_input_max_bytes, storage[ayni::kernel::Store::reflex_events],
                       capacities.reflex_event_capacity),
          reflex_engine(reflex_table, work, observations, calls),
          batch_max(ayni::kernel::transition_batch_max(capacities)) {}

    // Lifecycle transitions (contracts/domain/lifecycles.md, ADR-0016). The
    // C ABI has already checked the arguments.
    ayni_status_t start();
    ayni_status_t step(uint32_t max_items, uint32_t max_time_us, ayni_step_report_v1 &report);
    ayni_status_t begin_shutdown();
    // Why the kernel is RESTRICTED or FENCED; NONE otherwise.
    ayni_restriction_t restriction() const;
    // Writes the batches of the transitions from commit_seq first_seq up to
    // the latest into the log, right after the call that applied them; a
    // batch that does not fit, a defect, makes the kernel RESTRICTED.
    void journal(uint64_t first_seq);
    // The last commit_seq known durable (ayni_kernel_info_v1).
    uint64_t durable_seq() const;
    // Receipts use the current writer's acknowledged prefix, never the
    // public monotonic floor retained while a reload is incomplete.
    ayni_status_t intent_receipt(uint64_t principal_id, uint64_t command_id,
                                 ayni_intent_receipt_v1 &out) const;
    // ayni_submit_intent after its gate (ADR-0017, ADR-0031): applies intent
    // with one sample of the kernel, then, for a new revocation that took no
    // transition, its pending revoke when storage admits one, and journals
    // what was taken. out_committed says whether a transition was taken;
    // AYNI_INTERNAL may have taken one too (ayni.h).
    ayni_status_t submit_intent(const ayni::kernel::Intent &intent,
                                ayni::kernel::IntentOutcome &outcome,
                                ayni::kernel::IntentFailure &failure, bool &out_committed);

    // Caller code runs inside the kernel: functions that change the kernel
    // return AYNI_INVALID_STATE (ayni.h).
    bool in_callback() const { return callbacks.active(); }

    // Owner-thread temporary memory; const queries may borrow it synchronously.
    // This access changes no catalog, Task, Grant or other domain fact.
    ayni::foundation::Workspace &scratch_workspace() const { return workspace; }

    // Owner-thread read-only views, valid until the next mutation on this Kernel.
    const ayni::kernel::WorkModel &model() const { return work; }
    // Trusted registrations are borrowed read-only; only configure() inserts them.
    const ayni::kernel::Catalogs &catalog() const { return catalogs; }
    // Volatile call diagnostics; the scheduler owns every transition.
    const ayni::kernel::CallRuntime &call_view() const { return calls; }
    // Admission facts are read-only; health changes use set_resource_health().
    const ayni::resources::ResourceAdmission &resource_view() const { return resources; }
    // Live permit facts remain owned by execution control and resource admission.
    const ayni::resources::PermitTable &permit_view() const { return permits; }
    // Observation reads retain their freshness checks; publication is a separate operation.
    const ayni::knowledge::ObservationTable &observation_view() const { return observations; }
    // Any-thread diagnostics read only the transport's ingress-locked state.
    const ayni::kernel::ObservationIngress &observation_ingress_view() const {
        return observation_ingress;
    }
    // Any-thread lock-free diagnostics for triple-buffered latest observation slots.
    const ayni::kernel::ObservationSlots &observation_slots_view() const {
        return observation_slots;
    }
    // Lifecycle and writer diagnostics have one owner; these never expose mutable state.
    ayni_lifecycle_t state() const { return lifecycle; }
    // Recovered boot identity; zero before recovery establishes it.
    uint64_t boot_epoch() const { return boot.boot_epoch(); }
    // Active writer boot identity, used with its own durable sequence for receipts.
    uint64_t writer_boot_epoch() const { return log.boot_epoch(); }
    // Current writer acknowledged prefix; never the monotonic reload diagnostic floor.
    uint64_t writer_durable_seq() const { return log.durable_seq(); }
    // True when the writer has no queued batch or outstanding request.
    bool writer_idle() const { return log.idle(); }
    // Last complete local snapshot generation, for privacy completion/capacity diagnostics.
    uint64_t snapshot_generation() const { return maintenance.generation(); }
    // Number of intents the single decision buffer can stage; bounds registration.
    uint32_t decision_capacity() const { return decisions.intent_capacity(); }
    // Copy all Kernel diagnostics into an already validated caller value.
    void describe(ayni_kernel_info_v1 &out) const;
    // Fresh Host clock and wake access preserve lifetime without exposing the Host table.
    // Nonblocking Host sample decoded once per scheduling/intent frontier.
    ayni::work::CalendarClock calendar_clock(uint64_t monotonic_us) const;
    uint64_t now_us() const { return host.monotonic_now_us(host.ctx); }
    // Notify the owner through the immutable Host port; safe for ingress producers.
    void wake() const { host.wake(host.ctx); }

    // Configuration operations run only after the ABI registration gate. Typed
    // records were decoded at the boundary; catalogs retain their own invariants.
    ayni_status_t configure(const ayni::authority::Principal &principal);
    // Insert the trusted agent identity before startup; no instance is created yet.
    ayni_status_t configure_agent(uint64_t agent_id);
    // Register the decoded type and its borrowed callbacks before startup.
    ayni_status_t configure(const ayni::catalog::GoalType &type);
    // Register a decoded strategy whose callback lifetime belongs to the caller.
    ayni_status_t configure(const ayni::catalog::Strategy &strategy);
    // Copy an already validated authority domain using its canonical encoding.
    ayni_status_t configure(const ayni_capability_desc_v1 &capability,
                            const ayni::catalog::DomainEncoding &encoding);
    // Copy trusted device inventory; its catalog owns uniqueness and capacity.
    ayni_status_t configure(const ayni::catalog::Device &device);
    // Copy trusted component inventory; parent references resolve at startup.
    ayni_status_t configure(const ayni::catalog::Component &component);
    // Copy a decoded resource; admission owns later health and claim state.
    ayni_status_t configure(const ayni::catalog::Resource &resource);
    // Copy a trusted source description; no observation is published by registration.
    ayni_status_t configure(const ayni::catalog::ObservationSource &source);
    // Copy the decoded call contract; no capability is granted by registration.
    ayni_status_t configure(const ayni::catalog::CallCapability &capability,
                            const ayni_business_contract_v1 *definition = nullptr);
    // Copy a validated offer; callback contexts remain caller-owned.
    ayni_status_t configure(const ayni::catalog::Provider &provider,
                            const ayni::catalog::ProviderBusinessDefinitions &definitions = {});
    // Copy a trusted binding; cross-catalog references resolve at startup.
    ayni_status_t configure(const ayni::catalog::Binding &binding);
    // Catalog owns adopted immutable metadata; no backend callback or implicit Grant.
    ayni_status_t configure_usage(ayni_string_v1 summary, const ayni_tool_usage_v1 &usage);
    // These existing descriptions retain their domain-owned decoders and error order.
    ayni_status_t configure(const ayni_executor_v1 &executor);
    // Register the execution binding with its existing domain validation order.
    ayni_status_t configure(const ayni_execution_binding_v1 &binding);
    // Register a Scene ceiling; registration does not grant execution authority.
    ayni_status_t configure(const ayni_scene_desc_v1 &scene);
    // Register a static ReflexRule; cross-catalog references resolve at ayni_start().
    ayni_status_t configure(const ayni_reflex_rule_v1 &rule);
    // RUNNING owner-thread volatile toggle for one ReflexRule (0 WAL, ADR-0112).
    ayni_status_t set_reflex_enabled(ayni_string_v1 rule_id, uint32_t enabled);
    // Owner-thread diagnostics and bounded event ring drain for ReflexRules.
    ayni_status_t get_reflex_info(ayni_string_v1 rule_id, ayni_reflex_info_v1 &out) const;
    ayni_status_t take_reflex_event(ayni_reflex_event_v1 &out);
    // Owner sessions and volatile publications; persistence still goes through submit_intent.
    ayni_status_t open_session(const ayni::authority::Principal &principal, ayni_channel_t channel,
                               uint64_t &out_session_id);
    // Retire one live session identity; this changes no persistent authorization.
    ayni_status_t close_session(uint64_t session_id);
    // Change the resource-owned health fact and wake only on actual change.
    ayni_status_t set_resource_health(uint32_t index, ayni_resource_health_t health);
    // Publish with a fresh Host time and wake on acceptance; no persistence is implied.
    ayni_status_t publish_observation(const ayni::knowledge::ObservationInput &observation);
    // Any-thread producer operations; never read owner lifecycle or work facts.
    ayni_status_t post_observation(uint64_t boot_epoch,
                                   const ayni::knowledge::ObservationInput &input);
    // Any-thread lock-free latest-wins publication into per-source triple buffers (ADR-0112).
    ayni_status_t publish_observation_latest(uint64_t boot_epoch,
                                             const ayni::knowledge::ObservationInput &input);
    ayni_status_t report_observation_gap(uint64_t boot_epoch,
                                         const ayni::catalog::CatalogId &source, uint32_t version,
                                         ayni::knowledge::ObservationBoundary boundary);
    // Any-thread ingress, using only immutable Host ports and the ingress lock.
    ayni_status_t post_local_input(const ayni::catalog::CatalogId &source);

  private:
    friend class ayni::kernel::KernelTestProbe;

    // Sole owner of arena-backed temporary leases. Views in model/catalogs borrow
    // this object; producers never access it and no lease crosses an API return.
    mutable ayni::foundation::Workspace workspace;
    // Copy of the caller's table, with fields an older caller lacks left
    // zeroed. ctx and the functions stay caller-owned (see ayni_host_v1).
    ayni_host_v1 host;
    ayni_lifecycle_t lifecycle = AYNI_LIFECYCLE_CONFIGURING;
    ayni::kernel::CallbackGate callbacks; // Owns the owner-thread ABI callback boundary.
    // Filled by ayni_post_event and drained by ayni_step (ADR-0021).
    ayni::kernel::EventIngress ingress;
    // Events ayni_step took in RUNNING that no goal took, and events it
    // discarded outside RUNNING or left queued on entering STOPPED.
    uint64_t events_unmatched = 0;
    uint64_t events_discarded = 0;
    // One identity allocator shared by the distinct Host/Provider delivery pools;
    // transport slots own no counter that could collide after reload or reuse.
    ayni::kernel::AttemptSequence attempts;
    ayni::kernel::CompletionPool completions;
    ayni::kernel::WriteSchedule writes;
    // Every registry the Host fills in CONFIGURING; read-only from ayni_start.
    ayni::kernel::Catalogs catalogs;
    ayni::kernel::WorkModel work;
    ayni::kernel::WorkReplay replay;
    ayni::kernel::BootSequence boot;
    ayni::kernel::CommitLog log;
    // Compacts the current-schema log into a snapshot; while
    // it runs it holds the storage buffer (ADR-0027).
    ayni::kernel::StorageMaintenance maintenance;
    // What the strategies read beyond the tasks and write the decisions
    // ayni_step applies into (ADR-0022, ADR-0024).
    ayni::kernel::DecisionBuffer decisions;
    // The log's size against its limit and the free medium (C-STORE-011).
    ayni::kernel::StorageBudget budget;
    // Runtime allocation and wait state owned by resources/admission; no Task/Grant copy.
    ayni::resources::ResourceAdmission resources;
    // Provider workers post through this independent reliable pool; Host controls keep theirs.
    ayni::kernel::ProviderMailbox provider_completions;
    ayni::resources::PermitTable permits; // Resource-owned live leases; zero-capacity by default.
    // Executor answers posted from any thread, three request lanes per executor (ADR-0052).
    ayni::kernel::ExecutorMailbox executor_completions;
    // Live facts borrow static source descriptions from the catalogs.
    ayni::knowledge::ObservationTable observations;
    ayni::kernel::ObservationIngress
        observation_ingress; // Owns Host sample queue/gap lanes, no knowledge facts.
    // Lock-free triple-buffered latest observation slots drained at step start (ADR-0112).
    ayni::kernel::ObservationSlots observation_slots;
    // Bounded cross-owner call coordination; workers only access the two mailboxes.
    ayni::kernel::CallRuntime calls;
    // Registered static ReflexRules and asynchronous bounded event ring (ADR-0112).
    ayni::kernel::ReflexTable reflex_table;
    // Synchronous owner-thread evaluator bridging observations and CallRuntime (ADR-0112).
    ayni::kernel::ReflexEngine reflex_engine;

    // The largest batch one transition writes (work_journal.h).
    uint32_t batch_max;
    // Whether a transition may start now: the medium has room, no
    // compaction is pending or running, and the log has room for its batch.
    bool storage_has_room() const;
    // AYNI_OK, or AYNI_STORAGE_FAILED when the medium is short of space
    // (StorageBudget::admission()).
    ayni_status_t storage_admission_status() const;

  private:
    void settle_recovery();
    void enter_running();
    void enter_restricted(ayni_restriction_t reason);
    // Owner thread: whether a transition may start now.
    bool has_room() const { return storage_has_room(); }
    // Enters FENCED after the RUNNING writer exhausts its retries.
    void settle_log();
    // Explicit call-local context; never stored in Kernel or retained by a service.
    using StepContext = ayni::kernel::StepContext;
    // Each storage owner may advance once, sharing the call's item budget.
    void advance_recovery_and_maintenance(StepContext &progress);
    // Prioritize accepted controls, then bounded descendant cleanup.
    void apply_revocations(StepContext &progress);
    // Apply due work then decisions against this call's fixed frontiers.
    ayni::kernel::CallRuntime::AcceptedCalls apply_ready_work(StepContext &progress);
    // Give fresh Calls journal-only preparation after inputs and before the next append.
    void prepare_accepted_calls(StepContext &, const ayni::kernel::CallRuntime::AcceptedCalls &);
    // Reliable results and cancellation precede fresh strategy requests.
    void apply_calls(StepContext &progress);
    // One mapping of Kernel/writer lifecycle to permitted call operations.
    ayni::kernel::CallRuntime::Mode call_mode() const;
    // Drain only inputs queued before this phase, counting each against the budget.
    void apply_observation_gaps(ayni::kernel::StepContext &progress);
    void drain_observation_slots(ayni::kernel::StepContext &progress);
    void evaluate_reflexes(ayni::kernel::StepContext &progress);
    void take_queued_observations(ayni::kernel::StepContext &progress);
    void take_queued_inputs(StepContext &progress);
    // Report pending work and deadlines even after the budget is exhausted.
    void report_step(StepContext &progress, ayni_step_report_v1 &report);
    // Transfer a matched durable result from its volatile deny/pin to Grant facts.
    bool handoff_revocation(const ayni::kernel::PendingRevoke &pending);
    // Retire controls acknowledged by the current writer's exact boot and sequence.
    bool retire_durable_revocations();
    // Reconcile full command identities after the entire recovered prefix validates.
    bool reconcile_recovered_revocations();
    // Whether a control or RUNNING cleanup can progress with available journal space.
    bool revocation_ready() const;
    void enter_fenced(ayni_restriction_t reason);
    // Takes the outcome of a maintenance run that ended.
    void settle_maintenance();
    // Starts compaction at the durable boundary while RUNNING.
    void begin_maintenance();
    // Rereads the free medium and starts a compaction that is due.
    void refresh_storage();
    // A privacy cursor may progress after its durable source barrier and external release gates.
    bool privacy_ready() const;
    // A maintenance run holds the storage buffer.
    bool maintaining() const;
    // When the next paced storage request may start (ayni_step_report_v1).
    uint64_t storage_deadline() const;
    // Whether ayni_step appends the batches waiting.
    bool appends() const;
    // Whether ayni_step takes events in the current state (ADR-0021).
    bool takes_events() const;
    bool takes_event_now() const;
    void handle_local_input(const ayni::catalog::CatalogId &source, uint64_t now_us,
                            ayni::work::InputResumeBoundary input);
    // Each execution binding names a declared Scene at its version; Scenes are
    // authority's, so Catalogs::close cannot check this. No callbacks.
    ayni_status_t validate_execution_scenes() const;

    // Runs item, one owner item that takes at most one transition (ADR-0051):
    // an applied transition is journaled right away, a defect makes the
    // kernel RESTRICTED with its restriction so nothing of it is stored, and
    // none journals nothing. Returns whether the item applied.
    template <typename Item> bool run_transition(Item item) {
        using End = ayni::kernel::ItemEnd;
        const uint64_t first_seq = work.transitions.next_commit_seq();
        const End end = item();
        if (end.kind == End::Kind::defect) {
            enter_restricted(end.restriction);
        } else if (end.kind == End::Kind::applied) {
            journal(first_seq);
        }
        return end.kind == End::Kind::applied;
    }

    // durable_seq() as of the last fence, which a reload must not go below.
    uint64_t durable_floor_ = 0;
    ayni_restriction_t restriction_ = AYNI_RESTRICTION_NONE;
    bool log_started_ = false; // the log has the storage buffer
};

#endif // AYNI_CORE_KERNEL_KERNEL_STATE_H
