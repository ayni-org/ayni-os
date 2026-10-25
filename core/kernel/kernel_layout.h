// Placement of the kernel and its bounded stores inside the caller's memory
// block (ADR-0014). Computed from the capacities alone, so
// ayni_kernel_memory_requirements and ayni_create always agree.

#ifndef AYNI_CORE_KERNEL_KERNEL_LAYOUT_H
#define AYNI_CORE_KERNEL_KERNEL_LAYOUT_H

#include "kernel/kernel_state.h"

#include <cstddef>

namespace ayni::kernel {

struct KernelLayout {
    std::size_t ingress_offset;
    std::size_t completions_offset;
    std::size_t storage_buffer_offset;
    std::size_t agents_offset;
    std::size_t goal_types_offset;
    std::size_t goals_offset;
    std::size_t sessions_offset;
    std::size_t tasks_offset;
    std::size_t strategies_offset;
    std::size_t decisions_offset;
    std::size_t commands_offset;
    std::size_t capabilities_offset;
    std::size_t grants_offset;
    std::size_t goal_authority_offset;       // Byte offset of Goal-owned authority storage.
    std::size_t task_authority_offset;       // Byte offset of Task-owned admission references.
    std::size_t resource_catalog_offset;     // Immutable descriptions, no live state.
    std::size_t resource_states_offset;      // Live occupancy/health, never persistent.
    std::size_t resource_requests_offset;    // Bounded pending/held resource groups.
    std::size_t call_capabilities_offset;    // Copied execution contracts.
    std::size_t providers_offset;            // Trusted offers and callback descriptors.
    std::size_t bindings_offset;             // Immutable exact implementation selections.
    std::size_t calls_offset;                // Work-owned retained call rows and byte pools.
    std::size_t provider_completions_offset; // Separate Provider delivery capacity.
    std::size_t call_runtime_offset;         // Volatile I/O ownership and callback-copy buffers.
    std::size_t devices_offset;              // Copied immutable device identities.
    std::size_t components_offset;           // Copied component-to-device relationships.
    std::size_t size;
    std::size_t alignment; // a power of two
};

// The capacities are within the ayni.h maxima, so no size overflows even
// with a 32-bit size_t.
KernelLayout layout_for(const KernelCapacities &capacities);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_KERNEL_LAYOUT_H
