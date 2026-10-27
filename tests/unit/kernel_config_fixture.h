// Shared read-only conversion used by white-box tests and Native measurements.
#ifndef AYNI_TESTS_UNIT_KERNEL_CONFIG_FIXTURE_H
#define AYNI_TESTS_UNIT_KERNEL_CONFIG_FIXTURE_H
#include "kernel/service/persistence/work_journal.h"
#include <ayni/observations.h>
namespace ayni::test {
// White-box: the capacities ayni_create reads from config, which covers
// every field.
inline ayni::kernel::KernelCapacities capacities_of(const ayni_config_v1 &config) {
    return {config.ingress_capacity,
            config.event_body_max_bytes,
            config.completion_slot_count,
            config.storage_buffer_bytes,
            config.agent_capacity,
            config.goal_type_capacity,
            config.goal_capacity,
            config.goal_parameter_max_bytes,
            config.session_capacity,
            config.task_capacity,
            config.strategy_capacity,
            config.policy_intent_capacity,
            config.task_step_capacity,
            config.task_checkpoint_max_bytes,
            config.command_capacity,
            config.capability_capacity,
            config.authority_scope_max_bytes,
            config.grant_capacity,
            config.grant_delegation_depth_max,
            config.goal_requirement_capacity,
            config.resource_capacity,
            config.resource_request_capacity,
            config.call_capability_capacity,
            config.provider_capacity,
            config.binding_capacity,
            config.call_capacity,
            config.call_input_max_bytes == 0U ? AYNI_CALL_INPUT_BYTES_DEFAULT
                                              : config.call_input_max_bytes,
            config.call_result_max_bytes == 0U ? AYNI_CALL_RESULT_BYTES_DEFAULT
                                               : config.call_result_max_bytes,
            config.device_capacity,
            config.component_capacity,
            config.executor_capacity,
            config.execution_binding_capacity,
            config.scene_capacity,
            config.memory_capacity,
            config.memory_body_max_bytes == 0 ? AYNI_MEMORY_BODY_BYTES_DEFAULT
                                              : config.memory_body_max_bytes,
            config.memory_dependency_capacity == 0 ? AYNI_MEMORY_DEPENDENCY_CAPACITY_DEFAULT
                                                   : config.memory_dependency_capacity,
            config.observation_source_capacity,
            config.observation_ingress_capacity != 0  ? config.observation_ingress_capacity
            : config.observation_source_capacity != 0 ? AYNI_OBSERVATION_INGRESS_CAPACITY_DEFAULT
                                                      : 0};
}

// White-box: the smallest storage buffer ayni_create accepts with config's
// other capacities, which holds the largest batch a transition writes.
inline uint32_t smallest_storage_buffer(const ayni_config_v1 &config) {
    return ayni::kernel::storage_buffer_min(capacities_of(config));
}

} // namespace ayni::test
#endif
