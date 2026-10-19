// Representation of ayni_kernel, shared by the kernel sources and white-box
// unit tests. Nothing outside core/kernel and tests/ may include it.

#ifndef AYNI_CORE_KERNEL_KERNEL_STATE_H
#define AYNI_CORE_KERNEL_KERNEL_STATE_H

#include "kernel/boot/boot_sequence.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/ingress/event_queue.h"
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
        : host(host_table),
          ingress(storage.ingress, capacities.ingress_capacity, capacities.event_body_max_bytes),
          completions(storage.completions, capacities.completion_slot_count, host),
          boot(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes),
          work(capacities, limits, storage) {}

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
    // Filled by ayni_post_event and drained by ayni_step (MA-004).
    ayni::kernel::EventQueue ingress;
    ayni::kernel::CompletionPool completions;
    ayni::kernel::BootSequence boot;

    // The work model (ADR-0017), started when the kernel enters RUNNING.
    ayni::kernel::WorkModel work;

  private:
    void settle_recovery();
    void enter_running();
};

#endif // AYNI_CORE_KERNEL_KERNEL_STATE_H
