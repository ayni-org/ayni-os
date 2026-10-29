/* Reproducible ABI arena profiles; Native numbers are not ESP32 measurements. */
#include "measurement_profiles.h"
#include <stdio.h>

static int measure(const char *label, const ayni_config_v1 *config) {
    size_t bytes = 0, alignment = 0;
    const ayni_status_t status = ayni_kernel_memory_requirements(config, &bytes, &alignment);
    printf("%s: status=%d bytes=%zu alignment=%zu\n", label, (int)status, bytes, alignment);
    return status != AYNI_OK;
}
int main(void) {
    /* Every capacity at its _DEFAULT, including memory; optional call/execution
     * stores off: a C copy
     * of runtime/common default_config() (ADR-0053), whose C++ header this
     * C99 tool cannot include. Change both together. */
    ayni_config_v1 config = {0};
    config.struct_size = sizeof(config);
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.ingress_capacity = AYNI_INGRESS_CAPACITY_DEFAULT;
    config.event_body_max_bytes = AYNI_EVENT_BODY_MAX_BYTES_DEFAULT;
    config.completion_slot_count = AYNI_COMPLETION_SLOT_COUNT_DEFAULT;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    config.agent_capacity = AYNI_AGENT_CAPACITY_DEFAULT;
    config.goal_type_capacity = AYNI_GOAL_TYPE_CAPACITY_DEFAULT;
    config.goal_capacity = AYNI_GOAL_CAPACITY_DEFAULT;
    config.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_DEFAULT;
    config.session_capacity = AYNI_SESSION_CAPACITY_DEFAULT;
    config.goal_depth_max = AYNI_GOAL_DEPTH_MAX_DEFAULT;
    config.goal_children_max = AYNI_GOAL_CHILDREN_MAX_DEFAULT;
    config.task_capacity = AYNI_TASK_CAPACITY_DEFAULT;
    config.strategy_capacity = AYNI_STRATEGY_CAPACITY_DEFAULT;
    config.policy_intent_capacity = AYNI_POLICY_INTENT_CAPACITY_DEFAULT;
    config.task_step_capacity = AYNI_TASK_STEP_CAPACITY_DEFAULT;
    config.task_checkpoint_max_bytes = AYNI_TASK_CHECKPOINT_MAX_BYTES_DEFAULT;
    config.command_capacity = AYNI_COMMAND_CAPACITY_DEFAULT;
    config.capability_capacity = AYNI_CAPABILITY_CAPACITY_DEFAULT;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT;
    config.grant_capacity = AYNI_GRANT_CAPACITY_DEFAULT;
    config.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_DEFAULT;
    config.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_DEFAULT;
    config.memory_capacity = AYNI_MEMORY_CAPACITY_DEFAULT;
    config.memory_body_max_bytes = AYNI_MEMORY_BODY_BYTES_DEFAULT;
    config.memory_dependency_capacity = AYNI_MEMORY_DEPENDENCY_CAPACITY_DEFAULT;
    int failed = measure("defaults", &config);
    config.memory_capacity = 0;
    config.ingress_capacity = 1;
    config.event_body_max_bytes = 1;
    config.completion_slot_count = 1;
    config.agent_capacity = 1;
    config.goal_type_capacity = 1;
    config.goal_capacity = 1;
    config.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_MIN;
    config.session_capacity = 1;
    config.goal_depth_max = AYNI_GOAL_DEPTH_MAX_MIN;
    config.goal_children_max = AYNI_GOAL_CHILDREN_MAX_MIN;
    config.task_capacity = 1;
    config.strategy_capacity = 1;
    config.policy_intent_capacity = 1;
    config.task_step_capacity = 1;
    config.task_checkpoint_max_bytes = AYNI_TASK_CHECKPOINT_MAX_BYTES_MIN;
    config.command_capacity = 1;
    config.capability_capacity = 1;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN;
    config.grant_capacity = 1;
    config.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_MIN;
    config.goal_requirement_capacity = 1;
    failed |= measure("minimum work", &config);
    config.memory_capacity = AYNI_MEMORY_CAPACITY_DEFAULT;
    config.agent_capacity = 2;
    config.goal_type_capacity = 4;
    config.goal_capacity = 8;
    config.goal_parameter_max_bytes = 512;
    config.session_capacity = 4;
    config.goal_depth_max = 2;
    config.goal_children_max = 4;
    config.task_capacity = 8;
    config.strategy_capacity = 2;
    config.policy_intent_capacity = 8;
    config.task_step_capacity = 16;
    config.task_checkpoint_max_bytes = 256;
    config.command_capacity = 32;
    config.capability_capacity = 4;
    config.authority_scope_max_bytes = 256;
    config.grant_capacity = 16;
    config.grant_delegation_depth_max = 4;
    config.goal_requirement_capacity = 4;
    config.ingress_capacity = 32;
    config.event_body_max_bytes = 128;
    config.completion_slot_count = 8;
    config.resource_capacity = 4;
    config.device_capacity = 2;
    config.component_capacity = 4;
    config.resource_request_capacity = 16;
    config.call_capability_capacity = 4;
    config.provider_capacity = 2;
    config.binding_capacity = 4;
    config.call_capacity = 16;
    config.business_contract_max_bytes = AYNI_BUSINESS_CONTRACT_BYTES_DEFAULT;
    config.call_input_max_bytes = 4096;
    config.call_result_max_bytes = 4096;
    failed |= measure("Native two-agent call profile", &config);
    config.observation_source_capacity = 4;
    config.scene_capacity = 2;
    config.executor_capacity = 1;
    config.execution_binding_capacity = 2;
    failed |= measure("Native two-agent execution profile", &config);
    config = ayni_measure_maximum_config();
    failed |= measure("Native maximum capacities (automatic storage)", &config);
    return failed;
}
