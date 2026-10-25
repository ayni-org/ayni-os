// Representation of ayni_kernel, shared by the kernel sources and white-box
// unit tests. Nothing outside core/kernel and tests/ may include it.

#ifndef AYNI_CORE_KERNEL_KERNEL_STATE_H
#define AYNI_CORE_KERNEL_KERNEL_STATE_H

#include "catalog/binding/binding_registry.h"
#include "kernel/boot/boot_sequence.h"
#include "kernel/call_runtime.h"
#include "kernel/commit/commit_log.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/completion/provider_mailbox.h"
#include "kernel/ingress/event_ingress.h"
#include "kernel/intent/decision.h"
#include "kernel/intent/work_model.h"
#include "kernel/kernel_config.h"
#include "kernel/storage/storage_budget.h"
#include "kernel/storage/storage_maintenance.h"
#include "kernel/work_journal.h"
#include "kernel/work_replay.h"
#include "resources/admission/resource_admission.h"

#include <ayni/ayni.h>

#include <cstdint>

// Lives at the start of the caller's memory block; the stores follow it at
// the offsets of kernel_layout.h.
struct ayni_kernel {
    ayni_kernel(const ayni_host_v1 &host_table, const ayni::kernel::KernelCapacities &capacities,
                const ayni::kernel::KernelLimits &limits,
                const ayni::kernel::KernelStorage &storage)
        : host(host_table), ingress(storage.ingress, capacities.ingress_capacity,
                                    capacities.event_body_max_bytes, host),
          completions(storage.completions, capacities.completion_slot_count, host, attempts),
          writes(limits.storage_write_interval_us), work(capacities, limits, storage), replay(work),
          boot(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes, work,
               replay, writes),
          log(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes, writes),
          maintenance(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes,
                      work, writes),
          decisions(storage.decisions,
                    {capacities.policy_intent_capacity, capacities.task_step_capacity,
                     capacities.task_checkpoint_max_bytes}),
          budget(capacities, limits),
          resource_catalog(storage.resource_catalog, capacities.resource_capacity),
          resources(resource_catalog, storage.resource_states, capacities.resource_capacity,
                    storage.resource_requests, capacities.resource_request_capacity),
          devices(storage.devices, capacities.device_capacity, storage.components,
                  capacities.component_capacity),
          call_capabilities(storage.call_capabilities, capacities.call_capability_capacity),
          providers(storage.providers, capacities.provider_capacity),
          bindings(storage.bindings, capacities.binding_capacity),
          provider_completions(storage.provider_completions,
                               {capacities.call_capacity, capacities.call_result_max_bytes}, host),
          calls(storage.call_runtime,
                {capacities.call_capacity, capacities.call_input_max_bytes,
                 capacities.call_result_max_bytes},
                work, {call_capabilities, providers, bindings, resource_catalog}, resources,
                provider_completions, attempts, host),
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

    // Caller code runs inside the kernel: functions that change the kernel
    // return AYNI_INVALID_STATE (ayni.h).
    bool in_callback() const { return work.calling_out; }

    // Copy of the caller's table, with fields an older caller lacks left
    // zeroed. ctx and the functions stay caller-owned (see ayni_host_v1).
    ayni_host_v1 host;
    ayni_lifecycle_t lifecycle = AYNI_LIFECYCLE_CONFIGURING;
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
    // Static trusted directory; invariant/representation owned by catalog/resource.
    ayni::catalog::ResourceRegistry resource_catalog;
    // Runtime allocation and wait state owned by resources/admission; no Task/Grant copy.
    ayni::resources::ResourceAdmission resources;
    // Catalog owners retain immutable execution declarations and trusted selection;
    // callback contexts belong to runtime, while no live call state lives here.
    ayni::catalog::DeviceCatalog devices; // Immutable inventory; no health or resource ownership.
    ayni::catalog::CallCapabilityRegistry call_capabilities;
    ayni::catalog::ProviderRegistry providers;
    ayni::catalog::BindingRegistry bindings;
    // Provider workers post through this independent reliable pool; Host controls keep theirs.
    ayni::kernel::ProviderMailbox provider_completions;
    // Bounded cross-owner call coordination; workers only access provider_completions.
    ayni::kernel::CallRuntime calls;

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
    // Call-local accounting; its definition lives with the step implementation.
    struct StepBudget;
    // Each storage owner may advance once, sharing the call's item budget.
    void advance_recovery_and_maintenance(StepBudget &progress);
    // Prioritize accepted controls, then bounded descendant cleanup.
    void apply_revocations(StepBudget &progress);
    // Apply due work then decisions against this call's fixed frontiers.
    void apply_ready_work(StepBudget &progress);
    // Reliable results and cancellation precede fresh strategy requests.
    void apply_calls(StepBudget &progress);
    // One mapping of Kernel/writer lifecycle to permitted call operations.
    ayni::kernel::CallRuntime::Mode call_mode() const;
    // Drain only inputs queued before this phase, counting each against the budget.
    void take_queued_inputs(StepBudget &progress);
    // Report pending work and deadlines even after the budget is exhausted.
    void report_step(const StepBudget &progress, ayni_step_report_v1 &report);
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

    // durable_seq() as of the last fence, which a reload must not go below.
    uint64_t durable_floor_ = 0;
    ayni_restriction_t restriction_ = AYNI_RESTRICTION_NONE;
    bool log_started_ = false; // the log has the storage buffer
};

#endif // AYNI_CORE_KERNEL_KERNEL_STATE_H
