// Sizing of the bounded stores the work model builds (ADR-0014): each
// function derives one store's limits from the validated capacities.
// layout_for sizes the store's region with them and WorkModel builds the
// store with the same ones, so the two cannot disagree and a store never
// reaches past its region (ADR-0051). The journal's batch budget and the
// snapshot read them too. A store sized by a single capacity takes it
// straight from KernelCapacities; the stores ayni_kernel builds itself are
// sized in kernel_layout.h.

#ifndef AYNI_CORE_KERNEL_CONFIG_STORE_LIMITS_H
#define AYNI_CORE_KERNEL_CONFIG_STORE_LIMITS_H

#include "authority/grant/grant_table.h"
#include "authority/principal/principal_registry.h"
#include "authority/scene/scene_table.h"
#include "kernel/command/command_canonical.h"
#include "kernel/command/command_table.h"
#include "kernel/config/kernel_config.h"
#include "work/call/call_table.h"
#include "work/goal/goal_authority.h"
#include "work/task/task_authority.h"
#include "work/task/task_table.h"

namespace ayni::kernel {

inline work::TaskTable::Limits task_limits(const KernelCapacities &capacities) {
    return {capacities.task_capacity, capacities.task_step_capacity,
            capacities.task_checkpoint_max_bytes};
}

// One window per principal the registry holds. Each mapping keeps the exact
// request of the largest authority command (ADR-0030, ADR-0037), and every
// Grant may have one pending revocation (ADR-0031).
inline CommandTable::Limits command_limits(const KernelCapacities &capacities) {
    return {authority::PrincipalRegistry::capacity, capacities.command_capacity,
            exact_command_max_bytes(capacities.authority_scope_max_bytes,
                                    capacities.goal_parameter_max_bytes,
                                    capacities.goal_requirement_capacity),
            capacities.grant_capacity};
}

inline authority::GrantTable::Limits grant_limits(const KernelCapacities &capacities) {
    return {capacities.grant_capacity, capacities.authority_scope_max_bytes,
            capacities.grant_delegation_depth_max};
}

inline work::GoalAuthorityTable::Limits goal_authority_limits(const KernelCapacities &capacities) {
    return {capacities.goal_capacity, capacities.goal_requirement_capacity,
            capacities.authority_scope_max_bytes};
}

inline work::TaskAuthorityTable::Limits task_authority_limits(const KernelCapacities &capacities) {
    return {capacities.task_capacity, capacities.goal_requirement_capacity};
}

inline work::CallTable::Limits call_limits(const KernelCapacities &capacities) {
    return {capacities.call_capacity, capacities.authority_scope_max_bytes,
            capacities.call_input_max_bytes, capacities.call_result_max_bytes};
}

inline authority::Scenes::Limits scene_limits(const KernelCapacities &capacities) {
    return {capacities.scene_capacity, capacities.authority_scope_max_bytes};
}

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_CONFIG_STORE_LIMITS_H
