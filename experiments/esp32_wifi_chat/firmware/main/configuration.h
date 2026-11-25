#pragma once
#include "harness.h"
#include "protocol.h"
#include <ayni/ayni.h>
#include <ayni/strategies/builtin.h>
namespace wifichat {
// ADR-0114: host P99 ~0.54 ms, eightfold headroom rounded up; cooperative, not MCU WCET.
inline constexpr uint32_t owner_step_units = 16, owner_step_budget_us = 5000;
inline constexpr uint32_t goal_slots = 4, task_slots = 4, grant_slots = 20, step_slots = 64;
// Radio/TLS XiP and complete speech buffers share the board's 8 MiB PSRAM.
inline constexpr uint32_t call_slots = 32;
inline constexpr uint32_t unresolved_op_slots = 16;
static_assert(step_slots <= AYNI_TASK_STEP_CAPACITY_MAX, "Profile exceeds public task step limit");
// No runtime-private default helper is imported. These are public deployment capacities.
// call_input_bytes and call_result_bytes are the largest canonical encodings the registered
// contracts measured for themselves, so Core sizes its Call strides from them.
inline ayni_config_v1 chat_config(uint32_t call_input_bytes, uint32_t call_result_bytes) {
    ayni_config_v1 c{};
    c.struct_size = sizeof(c);
    c.abi_major = AYNI_ABI_MAJOR;
    c.abi_minor = AYNI_ABI_MINOR;
    c.ingress_capacity = AYNI_INGRESS_CAPACITY_DEFAULT;
    c.event_body_max_bytes = AYNI_EVENT_BODY_MAX_BYTES_DEFAULT;
    c.completion_slot_count = AYNI_COMPLETION_SLOT_COUNT_DEFAULT;
    c.storage_buffer_bytes = 128 * 1024;
    c.agent_capacity = 1;
    c.goal_type_capacity =
        6; // Chat, diagnostic, two checks, reply speech and independent operation.
    c.goal_capacity = goal_slots;
    // ToolLoop parameters and checkpoints hold only body references (ADR-0096).
    c.goal_parameter_max_bytes = ayni::strategies::ToolLoop::checkpoint_bytes;
    c.session_capacity = AYNI_SESSION_CAPACITY_DEFAULT;
    c.goal_depth_max = AYNI_GOAL_DEPTH_MAX_DEFAULT;
    c.goal_children_max = AYNI_GOAL_CHILDREN_MAX_DEFAULT;
    c.task_capacity = task_slots;
    c.strategy_capacity = 4; // ToolLoop, motor check Rules and parameterized reply-speech Rules.
    c.policy_intent_capacity = 1;
    c.task_step_capacity = step_slots;
    c.task_checkpoint_max_bytes = ayni::strategies::ToolLoop::checkpoint_bytes;
    c.command_capacity = 4;
    c.capability_capacity = 12;
    c.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT;
    c.grant_capacity = grant_slots;
    c.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_DEFAULT;
    c.goal_requirement_capacity = 2;
    // ADR-0100: shared Session opens a Core context; zero memory capacity disables it.
    c.memory_capacity = 1;
    c.memory_body_max_bytes = AYNI_MEMORY_BODY_BYTES_DEFAULT;
    c.memory_dependency_capacity = AYNI_MEMORY_DEPENDENCY_CAPACITY_DEFAULT;
    c.call_capacity = call_slots;
    c.unresolved_op_capacity = unresolved_op_slots;
    c.call_input_max_bytes = call_input_bytes;
    c.call_result_max_bytes = call_result_bytes;
    c.max_turns = harness::max_turns;
    c.call_capability_capacity = 12;
    c.provider_capacity = 4;
    c.binding_capacity = 12;
    c.resource_capacity = 4;
    c.scene_capacity = 1;
    c.executor_capacity = 1;
    c.execution_binding_capacity = 2;
    c.resource_request_capacity = 2;
    c.observation_source_capacity = 2;
    c.reflex_capacity = 2;
    c.reflex_event_capacity = 8;
    // Bounded RAM audit ring (ADR-0113): ~6.5 KiB PSRAM, drained to DiagnosticLog with 0 Flash
    // wear.
    c.audit_capacity = 64;
    c.storage_write_interval_us = 1000;
    return c;
}
} // namespace wifichat
