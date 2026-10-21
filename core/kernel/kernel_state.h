// Representation of ayni_kernel, shared by the kernel sources and white-box
// unit tests. Nothing outside core/kernel and tests/ may include it.

#ifndef AYNI_CORE_KERNEL_KERNEL_STATE_H
#define AYNI_CORE_KERNEL_KERNEL_STATE_H

#include "kernel/boot/boot_sequence.h"
#include "kernel/commit/commit_log.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/ingress/event_ingress.h"
#include "kernel/intent/decision.h"
#include "kernel/intent/work_model.h"
#include "kernel/kernel_config.h"
#include "kernel/storage_maintenance.h"
#include "kernel/work_journal.h"
#include "kernel/work_replay.h"

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
          completions(storage.completions, capacities.completion_slot_count, host),
          writes(limits.storage_write_interval_us), work(capacities, limits, storage), replay(work),
          boot(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes, work,
               replay, writes),
          log(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes, writes),
          maintenance(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes,
                      work, writes),
          decisions(storage.decisions,
                    {capacities.policy_intent_capacity, capacities.task_step_capacity,
                     capacities.task_checkpoint_max_bytes}),
          batch_max(ayni::kernel::transition_batch_max(capacities)),
          log_max_bytes_(limits.storage_log_max_bytes),
          snapshot_max_bytes_(ayni::kernel::snapshot_payload_max(capacities) +
                              ayni::durability::snapshot_header_size +
                              ayni::durability::snapshot_marker_size),
          journal_buffer_bytes_(capacities.storage_buffer_bytes) {}

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
    ayni::kernel::CompletionPool completions;
    ayni::kernel::WriteSchedule writes;
    ayni::kernel::WorkModel work;
    ayni::kernel::WorkReplay replay;
    ayni::kernel::BootSequence boot;
    ayni::kernel::CommitLog log;
    ayni::kernel::StorageMaintenance maintenance;
    // What the strategies read beyond the tasks and write the decisions
    // ayni_step applies into (ADR-0022, ADR-0024).
    ayni::kernel::DecisionBuffer decisions;

    // The largest batch one transition writes (work_journal.h).
    uint32_t batch_max;
    ayni_status_t storage_admission_status() const;
    bool storage_has_room() const {
        return !storage_space_blocked_ && !maintenance_requested_ &&
               maintenance_mode_ == MaintenanceMode::none && log.has_room();
    }

  private:
    void settle_recovery();
    void enter_running();
    void enter_restricted(ayni_restriction_t reason);
    // Owner thread: whether a transition may start now.
    bool has_room() const { return storage_has_room(); }
    // Enters FENCED after the RUNNING writer exhausts its retries.
    void settle_log();
    void enter_fenced(ayni_restriction_t reason);
    void settle_maintenance();
    void begin_maintenance(bool migration);
    void refresh_storage();
    // Whether ayni_step appends the batches waiting.
    bool appends() const;
    // Whether ayni_step takes events in the current state (ADR-0021).
    bool takes_events() const;
    bool takes_event_now() const;
    void handle_local_input(const ayni::catalog::CatalogId &source, uint64_t now_us);

    enum class MaintenanceMode { none, runtime, migration };
    MaintenanceMode maintenance_mode_ = MaintenanceMode::none;
    bool maintenance_requested_ = false;
    bool storage_space_blocked_ = false;
    bool reloading_ = false;
    uint32_t writer_epoch_ = 0;
    uint64_t boot_epoch_current_ = 0;
    uint64_t generation_ = 0;
    uint32_t active_slot_ = 0;
    uint64_t log_bytes_ = 0;
    uint64_t durable_floor_ = 0;
    uint64_t log_max_bytes_;
    uint64_t snapshot_max_bytes_;
    uint64_t journal_buffer_bytes_;
    ayni_restriction_t restriction_ = AYNI_RESTRICTION_NONE;
    bool log_started_ = false; // the log has the storage buffer
};

#endif // AYNI_CORE_KERNEL_KERNEL_STATE_H
