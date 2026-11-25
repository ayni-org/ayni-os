// Placement of the kernel and its bounded stores inside the caller's memory
// block (ADR-0014). Computed from the capacities alone, so
// ayni_kernel_memory_requirements and ayni_create always agree.

#ifndef AYNI_CORE_KERNEL_KERNEL_LAYOUT_H
#define AYNI_CORE_KERNEL_KERNEL_LAYOUT_H

#include "kernel/config/kernel_config.h"
#include "kernel/ports/provider_mailbox.h"
#include "kernel/service/call/call_runtime.h"
#include "kernel/service/decision/decision.h"

#include <array>
#include <cstddef>

namespace ayni::kernel {
// Owner admission holds Call views and a Goal candidate while catalog codecs validate them.
std::size_t owner_workspace_size();

// One store's bytes in the caller's memory block: size bytes from offset,
// which is a multiple of alignment, a power of two. A store at zero capacity
// may have size 0.
struct ArenaRegion {
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t alignment = 1;
};

// The kernel at offset 0, then one region per Store in Store order, none
// overlapping another or the kernel.
struct KernelLayout {
    const ArenaRegion &operator[](Store store) const {
        return regions[static_cast<std::size_t>(store)];
    }

    std::array<ArenaRegion, store_count> regions{};
    std::size_t size = 0;      // From the block's start to the end of the last region.
    std::size_t alignment = 0; // The largest alignment of the kernel and its stores.
};

// The capacities are within the ayni.h maxima, so no size overflows even
// with a 32-bit size_t.
KernelLayout layout_for(const KernelCapacities &capacities);

// Where each store's memory starts in a block laid out by layout, memory
// being the block's start.
KernelStorage storage_at(void *memory, const KernelLayout &layout);

// Sizing of the stores ayni_kernel builds itself, from the validated
// capacities; layout_for and the kernel's constructor both read these, as
// config/store_limits.h does for the work model's stores.
inline DecisionBuffer::Limits decision_limits(const KernelCapacities &capacities) {
    return {capacities.policy_intent_capacity, capacities.task_step_capacity,
            capacities.task_checkpoint_max_bytes,
            capacities.memory_capacity == 0 ? 0 : capacities.memory_body_max_bytes};
}

inline CallRuntime::Limits call_runtime_limits(const KernelCapacities &capacities) {
    return {capacities.call_capacity, capacities.call_input_max_bytes,
            capacities.call_result_max_bytes};
}

inline ProviderMailbox::Limits provider_mailbox_limits(const KernelCapacities &capacities) {
    return {capacities.call_capacity + (capacities.unresolved_op_capacity > 0U ? 1U : 0U),
            capacities.call_result_max_bytes};
}

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_KERNEL_LAYOUT_H
