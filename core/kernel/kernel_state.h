// Representation of ayni_kernel, shared by the kernel sources and white-box
// unit tests. Nothing outside core/kernel and tests/ may include it.

#ifndef AYNI_CORE_KERNEL_KERNEL_STATE_H
#define AYNI_CORE_KERNEL_KERNEL_STATE_H

#include "kernel/boot/boot_sequence.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/ingress/event_queue.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// Validated capacities from ayni_config_v1; they fix the memory layout.
struct KernelCapacities {
    uint32_t ingress_capacity;
    uint32_t event_body_max_bytes;
    uint32_t completion_slot_count;
    uint32_t storage_buffer_bytes;
};

// The bounded stores that follow the kernel in the caller's memory block.
struct KernelStorage {
    void *ingress;
    void *completions;
    uint8_t *storage_buffer;
};

} // namespace ayni::kernel

// Lives at the start of the caller's memory block; the stores follow it at
// the offsets of kernel_layout.h.
struct ayni_kernel {
    ayni_kernel(const ayni_host_v1 &host_table, const ayni::kernel::KernelCapacities &capacities,
                const ayni::kernel::KernelStorage &storage)
        : host(host_table),
          ingress(storage.ingress, capacities.ingress_capacity, capacities.event_body_max_bytes),
          completions(storage.completions, capacities.completion_slot_count, host),
          boot(host, completions, storage.storage_buffer, capacities.storage_buffer_bytes) {}

    // Lifecycle transitions (contracts/domain/lifecycles.md, ADR-0016). The
    // C ABI has already checked the arguments.
    ayni_status_t start();
    ayni_status_t step(uint32_t max_items, uint32_t max_time_us, ayni_step_report_v1 &report);
    ayni_status_t begin_shutdown();

    // Copy of the caller's table, with fields an older caller lacks left
    // zeroed. ctx and the functions stay caller-owned (see ayni_host_v1).
    ayni_host_v1 host;
    ayni_lifecycle_t lifecycle = AYNI_LIFECYCLE_CONFIGURING;
    // Not NONE exactly while lifecycle is RESTRICTED.
    ayni_restriction_t restriction = AYNI_RESTRICTION_NONE;
    // Filled by ayni_post_event and drained by ayni_step (MA-004).
    ayni::kernel::EventQueue ingress;
    ayni::kernel::CompletionPool completions;
    ayni::kernel::BootSequence boot;

  private:
    void settle_recovery();
};

#endif // AYNI_CORE_KERNEL_KERNEL_STATE_H
