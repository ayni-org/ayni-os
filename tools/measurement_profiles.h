/* Native measurement inputs, not deployment defaults. Shared by the C99
 * arena query and the white-box benchmark so their maximum profile agrees. */
#ifndef AYNI_TOOLS_MEASUREMENT_PROFILES_H
#define AYNI_TOOLS_MEASUREMENT_PROFILES_H
#include <ayni/ayni.h>
#include <ayni/calls.h>
#include <ayni/execution.h>
#include <ayni/observations.h>
#include <ayni/resources.h>
#include <ayni/scenes.h>

/* All public capacity/size/structural limits at their maxima. Storage sizes
 * remain automatic; write interval is chosen by the measurement workload. */
static inline ayni_config_v1 ayni_measure_maximum_config(void) {
#ifdef __cplusplus
    ayni_config_v1 c = {};
#else
    ayni_config_v1 c = {0};
#endif
    c.struct_size = sizeof(c);
    c.abi_major = AYNI_ABI_MAJOR;
    c.abi_minor = AYNI_ABI_MINOR;
    c.ingress_capacity = AYNI_INGRESS_CAPACITY_MAX;
    c.event_body_max_bytes = AYNI_EVENT_BODY_MAX_BYTES_MAX;
    c.completion_slot_count = AYNI_COMPLETION_SLOT_COUNT_MAX;
    c.agent_capacity = AYNI_AGENT_CAPACITY_MAX;
    c.goal_type_capacity = AYNI_GOAL_TYPE_CAPACITY_MAX;
    c.goal_capacity = AYNI_GOAL_CAPACITY_MAX;
    c.goal_parameter_max_bytes = AYNI_GOAL_PARAMETER_MAX_BYTES_MAX;
    c.session_capacity = AYNI_SESSION_CAPACITY_MAX;
    c.goal_depth_max = AYNI_GOAL_DEPTH_MAX_MAX;
    c.goal_children_max = AYNI_GOAL_CHILDREN_MAX_MAX;
    c.task_capacity = AYNI_TASK_CAPACITY_MAX;
    c.strategy_capacity = AYNI_STRATEGY_CAPACITY_MAX;
    c.policy_intent_capacity = AYNI_POLICY_INTENT_CAPACITY_MAX;
    c.task_step_capacity = AYNI_TASK_STEP_CAPACITY_MAX;
    c.task_checkpoint_max_bytes = AYNI_TASK_CHECKPOINT_MAX_BYTES_MAX;
    c.command_capacity = AYNI_COMMAND_CAPACITY_MAX;
    c.capability_capacity = AYNI_CAPABILITY_CAPACITY_MAX;
    c.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX;
    c.grant_capacity = AYNI_GRANT_CAPACITY_MAX;
    c.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX;
    c.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_MAX;
    c.resource_capacity = AYNI_RESOURCE_CAPACITY_MAX;
    c.resource_request_capacity = AYNI_RESOURCE_REQUEST_CAPACITY_MAX;
    c.max_turns = AYNI_MAX_TURNS_MAX;
    c.call_capability_capacity = AYNI_CALL_CAPABILITY_CAPACITY_MAX;
    c.provider_capacity = AYNI_PROVIDER_CAPACITY_MAX;
    c.binding_capacity = AYNI_BINDING_CAPACITY_MAX;
    c.call_capacity = AYNI_CALL_CAPACITY_MAX;
    c.call_input_max_bytes = AYNI_CALL_INPUT_BYTES_MAX;
    c.call_result_max_bytes = AYNI_CALL_RESULT_BYTES_MAX;
    c.device_capacity = AYNI_DEVICE_CAPACITY_MAX;
    c.component_capacity = AYNI_COMPONENT_CAPACITY_MAX;
    c.observation_source_capacity = AYNI_OBSERVATION_SOURCE_CAPACITY_MAX;
    c.scene_capacity = AYNI_SCENE_CAPACITY_MAX;
    c.executor_capacity = AYNI_EXECUTOR_CAPACITY_MAX;
    c.execution_binding_capacity = AYNI_EXECUTION_BINDING_CAPACITY_MAX;
    c.memory_capacity = AYNI_MEMORY_CAPACITY_MAX;
    c.memory_body_max_bytes = AYNI_MEMORY_BODY_BYTES_MAX;
    c.memory_dependency_capacity = AYNI_MEMORY_DEPENDENCY_CAPACITY_MAX;
    c.business_contract_max_bytes = AYNI_BUSINESS_CONTRACT_BYTES_MAX;
    c.observation_ingress_capacity = AYNI_OBSERVATION_INGRESS_CAPACITY_MAX;
    return c;
}
#endif
