#include "kernel/kernel_layout.h"

namespace ayni::kernel {
namespace {

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1U) / alignment * alignment;
}

constexpr std::size_t larger(std::size_t a, std::size_t b) {
    return a > b ? a : b;
}

} // namespace

KernelLayout layout_for(const KernelCapacities &capacities) {
    KernelLayout layout{};
    layout.ingress_offset = align_up(sizeof(ayni_kernel), EventQueue::storage_alignment);
    const std::size_t ingress_end =
        layout.ingress_offset +
        EventQueue::storage_size(capacities.ingress_capacity, capacities.event_body_max_bytes);
    layout.completions_offset = align_up(ingress_end, CompletionPool::storage_alignment);
    layout.size =
        layout.completions_offset + CompletionPool::storage_size(capacities.completion_slot_count);
    layout.alignment = larger(alignof(ayni_kernel), larger(EventQueue::storage_alignment,
                                                           CompletionPool::storage_alignment));
    return layout;
}

} // namespace ayni::kernel
