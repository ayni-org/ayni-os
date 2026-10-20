// Representation of ayni_kernel, shared by the kernel sources and white-box
// unit tests. Nothing outside core/kernel and tests/ may include it.

#ifndef AYNI_CORE_KERNEL_KERNEL_STATE_H
#define AYNI_CORE_KERNEL_KERNEL_STATE_H

#include "kernel/boot/boot_sequence.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/ingress/event_ingress.h"
#include "kernel/intent/decision.h"
#include "kernel/intent/work_model.h"
#include "kernel/kernel_config.h"

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
          boot(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes),
          work(capacities, limits, storage),
          decisions(storage.policy_intents, capacities.policy_intent_capacity) {}

    // Lifecycle transitions (contracts/domain/lifecycles.md, ADR-0016). The
    // C ABI has already checked the arguments.
    ayni_status_t start();
    ayni_status_t step(uint32_t max_items, uint32_t max_time_us, ayni_step_report_v1 &report);
    ayni_status_t begin_shutdown();
    // Why the kernel is RESTRICTED; NONE in every other state. Recovery is
    // the only way into RESTRICTED so far.
    ayni_restriction_t restriction() const;

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
    ayni::kernel::BootSequence boot;

    // The work model (ADR-0017), started when the kernel enters RUNNING.
    ayni::kernel::WorkModel work;
    // Where the strategies write the decisions ayni_step applies (ADR-0022).
    ayni::kernel::DecisionBuffer decisions;

  private:
    void settle_recovery();
    void enter_running();
    // Whether ayni_step takes events in the current state (ADR-0021).
    bool takes_events() const;
    void handle_local_input(const ayni::catalog::CatalogId &source, uint64_t now_us);
};

#endif // AYNI_CORE_KERNEL_KERNEL_STATE_H
