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
    std::size_t policy_intents_offset;
    std::size_t size;
    std::size_t alignment; // a power of two
};

// The capacities are within the ayni.h maxima, so no size overflows even
// with a 32-bit size_t.
KernelLayout layout_for(const KernelCapacities &capacities);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_KERNEL_LAYOUT_H
