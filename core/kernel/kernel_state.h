// Representation of ayni_kernel, shared by the kernel sources and white-box
// unit tests. Nothing outside core/kernel and tests/ may include it.

#ifndef AYNI_CORE_KERNEL_KERNEL_STATE_H
#define AYNI_CORE_KERNEL_KERNEL_STATE_H

#include "authority/principal/principal_registry.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "foundation/kernel_id.h"
#include "kernel/boot/boot_sequence.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/ingress/event_queue.h"
#include "kernel/intent/intent.h"
#include "kernel/transition_clock.h"
#include "work/agent/agent_table.h"
#include "work/goal/goal_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// Validated capacities from ayni_config_v1; they fix the memory layout.
struct KernelCapacities {
    uint32_t ingress_capacity;
    uint32_t event_body_max_bytes;
    uint32_t completion_slot_count;
    uint32_t storage_buffer_bytes;
    uint32_t agent_capacity;
    uint32_t goal_type_capacity;
    uint32_t goal_capacity;
    uint32_t goal_parameter_max_bytes;
    uint32_t session_capacity;
};

// Validated limits from ayni_config_v1 that do not change the layout.
struct KernelLimits {
    uint32_t goal_depth_max;
    uint32_t goal_children_max;
};

// The bounded stores that follow the kernel in the caller's memory block.
struct KernelStorage {
    void *ingress;
    void *completions;
    uint8_t *storage_buffer;
    void *agents;
    void *goal_types;
    void *goals;
    void *sessions;
};

} // namespace ayni::kernel

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
          sessions(storage.sessions, capacities.session_capacity),
          goal_types(storage.goal_types, capacities.goal_type_capacity),
          agents(storage.agents, capacities.agent_capacity),
          goals(storage.goals, {capacities.goal_capacity, capacities.goal_parameter_max_bytes,
                                limits.goal_depth_max, limits.goal_children_max}) {}

    // Lifecycle transitions (contracts/domain/lifecycles.md, ADR-0016). The
    // C ABI has already checked the arguments.
    ayni_status_t start();
    ayni_status_t step(uint32_t max_items, uint32_t max_time_us, ayni_step_report_v1 &report);
    ayni_status_t begin_shutdown();
    // Why the kernel is RESTRICTED; NONE in every other state. Recovery is
    // the only way into RESTRICTED so far.
    ayni_restriction_t restriction() const;

    // Checks a decoded intent from step 3 of ayni_submit_intent on and, if
    // it passes, applies its transition (ADR-0017).
    ayni_status_t submit_intent(const ayni::kernel::Intent &intent,
                                ayni::kernel::IntentOutcome &outcome,
                                ayni::kernel::IntentFailure &failure);

    // Copy of the caller's table, with fields an older caller lacks left
    // zeroed. ctx and the functions stay caller-owned (see ayni_host_v1).
    ayni_host_v1 host;
    ayni_lifecycle_t lifecycle = AYNI_LIFECYCLE_CONFIGURING;
    // Filled by ayni_post_event and drained by ayni_step (MA-004).
    ayni::kernel::EventQueue ingress;
    ayni::kernel::CompletionPool completions;
    ayni::kernel::BootSequence boot;

    // The work model (ADR-0017). Registration fills principals, goal types
    // and agents in CONFIGURING; the rest changes only in RUNNING.
    ayni::authority::PrincipalRegistry principals;
    ayni::authority::SessionTable sessions;
    ayni::catalog::GoalTypeRegistry goal_types;
    ayni::work::AgentTable agents;
    ayni::work::GoalTable goals;
    // Started when the kernel enters RUNNING, from the boot epoch and batch.
    ayni::foundation::IdAllocator ids;
    ayni::kernel::TransitionClock transitions;

  private:
    void settle_recovery();
    void enter_running();
    ayni_status_t create_goal(const ayni::kernel::Intent &intent,
                              const ayni::work::GoalOrigin &origin,
                              ayni::kernel::IntentOutcome &outcome,
                              ayni::kernel::IntentFailure &failure);
    ayni_status_t change_goal(const ayni::kernel::Intent &intent,
                              ayni::kernel::IntentOutcome &outcome,
                              ayni::kernel::IntentFailure &failure);
};

#endif // AYNI_CORE_KERNEL_KERNEL_STATE_H
