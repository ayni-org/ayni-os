// The configuration every composition root starts from (ADR-0053): each
// capacity at its documented _DEFAULT, and the optional stores (resources,
// calls, devices, execution, Scenes, observation) off. A node changes only
// what it needs. The ABI has no default-config function, so this stays
// inside runtime/.

#ifndef AYNI_RUNTIME_COMMON_DEFAULT_CONFIG_H
#define AYNI_RUNTIME_COMMON_DEFAULT_CONFIG_H

#include <algorithm>
#include <ayni/ayni.h>
#include <ayni/calls.h>
#include <initializer_list>

namespace ayni::runtime {

inline ayni_config_v1 default_config() {
    ayni_config_v1 config{};
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
    // Each field not set here is a store that stays off at 0, or a limit
    // whose 0 takes its default.
    return config;
}

// The canonical input and result bytes one Call contract declares.
struct CallBytes {
    uint32_t input;
    uint32_t result;
};
inline CallBytes call_bytes(const ayni_call_contract_v1 &contract) {
    return {contract.input_text_limit, contract.result_text_limit};
}
// Sizes the Kernel's Call strides to the largest contracts the composition registers, so no
// composition writes its own byte estimate. Model bodies travel by reference (ADR-0096). A
// stride is at least one byte, because zero would select the 4 KiB default.
template <class Contracts> void fit_call_bytes(ayni_config_v1 &config, const Contracts &contracts) {
    config.call_input_max_bytes = 1;
    config.call_result_max_bytes = 1;
    for (const CallBytes &contract : contracts) {
        config.call_input_max_bytes = std::max(config.call_input_max_bytes, contract.input);
        config.call_result_max_bytes = std::max(config.call_result_max_bytes, contract.result);
    }
}
inline void fit_call_bytes(ayni_config_v1 &config, std::initializer_list<CallBytes> contracts) {
    fit_call_bytes<std::initializer_list<CallBytes>>(config, contracts);
}

} // namespace ayni::runtime

#endif // AYNI_RUNTIME_COMMON_DEFAULT_CONFIG_H
