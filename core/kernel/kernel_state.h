// Representation of ayni_kernel, shared by the kernel sources and white-box
// unit tests. Nothing outside core/kernel and tests/ may include it.

#ifndef AYNI_CORE_KERNEL_KERNEL_STATE_H
#define AYNI_CORE_KERNEL_KERNEL_STATE_H

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
};

} // namespace ayni::kernel

// Lives at the start of the caller's memory block; the queue and slot storage
// follow it at the offsets of kernel_layout.h.
struct ayni_kernel {
    ayni_kernel(const ayni_host_v1 &host_table, const ayni::kernel::KernelCapacities &capacities,
                void *ingress_storage, void *completion_storage)
        : host(host_table),
          ingress(ingress_storage, capacities.ingress_capacity, capacities.event_body_max_bytes),
          completions(completion_storage, capacities.completion_slot_count, host) {}

    // Copy of the caller's table, with fields an older caller lacks left
    // zeroed. ctx and the functions stay caller-owned (see ayni_host_v1).
    ayni_host_v1 host;
    ayni_lifecycle_t lifecycle = AYNI_LIFECYCLE_CONFIGURING;
    // Filled by ayni_post_event and drained by ayni_step (MA-005).
    ayni::kernel::EventQueue ingress;
    ayni::kernel::CompletionPool completions;
};

#endif // AYNI_CORE_KERNEL_KERNEL_STATE_H
