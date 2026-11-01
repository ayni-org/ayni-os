#pragma once
#include "protocol.h"
#include <ayni/ayni.h>
namespace usbchat {
inline constexpr uint32_t goal_slots = 4, task_slots = 2, grant_slots = 12;
// No runtime-private default helper is imported. These are public deployment capacities.
inline ayni_config_v1 chat_config() {
    ayni_config_v1 c{};
    c.struct_size = sizeof(c);
    c.abi_major = AYNI_ABI_MAJOR;
    c.abi_minor = AYNI_ABI_MINOR;
    c.ingress_capacity = AYNI_INGRESS_CAPACITY_DEFAULT;
    c.event_body_max_bytes = AYNI_EVENT_BODY_MAX_BYTES_DEFAULT;
    c.completion_slot_count = AYNI_COMPLETION_SLOT_COUNT_DEFAULT;
    c.storage_buffer_bytes = 128 * 1024;
    c.agent_capacity = 1;
    c.goal_type_capacity = 1;
    c.goal_capacity = goal_slots;
    c.goal_parameter_max_bytes = prompt_bytes;
    c.session_capacity = AYNI_SESSION_CAPACITY_DEFAULT;
    c.goal_depth_max = AYNI_GOAL_DEPTH_MAX_DEFAULT;
    c.goal_children_max = AYNI_GOAL_CHILDREN_MAX_DEFAULT;
    c.task_capacity = task_slots;
    c.strategy_capacity = 1;
    c.policy_intent_capacity = 1;
    c.task_step_capacity = 48;
    c.task_checkpoint_max_bytes = prompt_bytes + 16;
    c.command_capacity = 4;
    c.capability_capacity = 6;
    c.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT;
    c.grant_capacity = grant_slots;
    c.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_DEFAULT;
    c.goal_requirement_capacity = 1;
    c.memory_body_max_bytes = AYNI_MEMORY_BODY_BYTES_DEFAULT;
    c.memory_dependency_capacity = AYNI_MEMORY_DEPENDENCY_CAPACITY_DEFAULT;
    c.call_capacity = 32;
    c.call_input_max_bytes = prompt_bytes + 256;
    c.call_result_max_bytes = 8192;
    c.call_capability_capacity = 6;
    c.provider_capacity = 4;
    c.binding_capacity = 6;
    c.resource_capacity = 3;
    c.resource_request_capacity = 2;
    c.storage_write_interval_us = 1000;
    return c;
}
} // namespace usbchat
