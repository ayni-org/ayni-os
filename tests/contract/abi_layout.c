/*
 * Pins the published layout of the implemented ABI structs (C-VER-002): a
 * field may be appended, never moved, retyped or removed. A violation fails
 * the build. Pointer-sized fields are measured in sizeof(void *) so that the
 * same checks fit 32-bit targets; only the native build compiles them so far.
 */
#include <ayni/ayni.h>

#include <stddef.h>

#define AYNI_LAYOUT(name, condition) typedef char ayni_layout_##name[(condition) ? 1 : -1]
#define AYNI_PTR sizeof(void *)
/* A struct with a uint64_t field ends at a multiple of 8: every target
 * aligns uint64_t to 8 bytes. */
#define AYNI_ROUND8(size) (((size) + 7u) / 8u * 8u)

AYNI_LAYOUT(function_pointer_size, sizeof(void (*)(void)) == AYNI_PTR);

AYNI_LAYOUT(config_struct_size, offsetof(ayni_config_v1, struct_size) == 0);
AYNI_LAYOUT(config_abi_major, offsetof(ayni_config_v1, abi_major) == 4);
AYNI_LAYOUT(config_abi_minor, offsetof(ayni_config_v1, abi_minor) == 8);
AYNI_LAYOUT(config_ingress_capacity, offsetof(ayni_config_v1, ingress_capacity) == 12);
AYNI_LAYOUT(config_event_body_max_bytes, offsetof(ayni_config_v1, event_body_max_bytes) == 16);
AYNI_LAYOUT(config_completion_slot_count, offsetof(ayni_config_v1, completion_slot_count) == 20);
AYNI_LAYOUT(config_storage_buffer_bytes, offsetof(ayni_config_v1, storage_buffer_bytes) == 24);
AYNI_LAYOUT(config_agent_capacity, offsetof(ayni_config_v1, agent_capacity) == 28);
AYNI_LAYOUT(config_goal_type_capacity, offsetof(ayni_config_v1, goal_type_capacity) == 32);
AYNI_LAYOUT(config_goal_capacity, offsetof(ayni_config_v1, goal_capacity) == 36);
AYNI_LAYOUT(config_goal_parameter_max_bytes,
            offsetof(ayni_config_v1, goal_parameter_max_bytes) == 40);
AYNI_LAYOUT(config_session_capacity, offsetof(ayni_config_v1, session_capacity) == 44);
AYNI_LAYOUT(config_goal_depth_max, offsetof(ayni_config_v1, goal_depth_max) == 48);
AYNI_LAYOUT(config_goal_children_max, offsetof(ayni_config_v1, goal_children_max) == 52);
AYNI_LAYOUT(config_task_capacity, offsetof(ayni_config_v1, task_capacity) == 56);
AYNI_LAYOUT(config_size, sizeof(ayni_config_v1) == 60);

AYNI_LAYOUT(host_struct_size, offsetof(ayni_host_v1, struct_size) == 0);
AYNI_LAYOUT(host_ctx, offsetof(ayni_host_v1, ctx) == AYNI_PTR);
AYNI_LAYOUT(host_monotonic_now_us, offsetof(ayni_host_v1, monotonic_now_us) == 2 * AYNI_PTR);
AYNI_LAYOUT(host_wake, offsetof(ayni_host_v1, wake) == 3 * AYNI_PTR);
AYNI_LAYOUT(host_ingress_lock, offsetof(ayni_host_v1, ingress_lock) == 4 * AYNI_PTR);
AYNI_LAYOUT(host_ingress_unlock, offsetof(ayni_host_v1, ingress_unlock) == 5 * AYNI_PTR);
AYNI_LAYOUT(host_storage_append, offsetof(ayni_host_v1, storage_append) == 6 * AYNI_PTR);
AYNI_LAYOUT(host_storage_read_prefix, offsetof(ayni_host_v1, storage_read_prefix) == 7 * AYNI_PTR);
AYNI_LAYOUT(host_size, sizeof(ayni_host_v1) == 8 * AYNI_PTR);

AYNI_LAYOUT(attempt_boot_epoch, offsetof(ayni_attempt_ref_v1, boot_epoch) == 0);
AYNI_LAYOUT(attempt_request_id, offsetof(ayni_attempt_ref_v1, request_id) == 8);
AYNI_LAYOUT(attempt_size, sizeof(ayni_attempt_ref_v1) == 16);

AYNI_LAYOUT(result_struct_size, offsetof(ayni_completion_result_v1, struct_size) == 0);
AYNI_LAYOUT(result_outcome, offsetof(ayni_completion_result_v1, outcome) == 4);
AYNI_LAYOUT(result_attempt, offsetof(ayni_completion_result_v1, attempt) == 8);
AYNI_LAYOUT(result_value, offsetof(ayni_completion_result_v1, value) == 24);
AYNI_LAYOUT(result_size, sizeof(ayni_completion_result_v1) == 32);

AYNI_LAYOUT(bytes_data, offsetof(ayni_bytes_v1, data) == 0);
AYNI_LAYOUT(bytes_size, offsetof(ayni_bytes_v1, size) == AYNI_PTR);
AYNI_LAYOUT(bytes_struct_size, sizeof(ayni_bytes_v1) == 2 * AYNI_PTR);

AYNI_LAYOUT(batch_struct_size, offsetof(ayni_storage_batch_v1, struct_size) == 0);
AYNI_LAYOUT(batch_writer_epoch, offsetof(ayni_storage_batch_v1, writer_epoch) == 4);
AYNI_LAYOUT(batch_transition_id, offsetof(ayni_storage_batch_v1, transition_id) == 8);
AYNI_LAYOUT(batch_commit_seq, offsetof(ayni_storage_batch_v1, commit_seq) == 16);
AYNI_LAYOUT(batch_encoded, offsetof(ayni_storage_batch_v1, encoded) == 24);
AYNI_LAYOUT(batch_size, sizeof(ayni_storage_batch_v1) == 24 + 2 * AYNI_PTR);

AYNI_LAYOUT(info_struct_size, offsetof(ayni_kernel_info_v1, struct_size) == 0);
AYNI_LAYOUT(info_lifecycle, offsetof(ayni_kernel_info_v1, lifecycle) == 4);
AYNI_LAYOUT(info_restriction, offsetof(ayni_kernel_info_v1, restriction) == 8);
AYNI_LAYOUT(info_reserved, offsetof(ayni_kernel_info_v1, reserved) == 12);
AYNI_LAYOUT(info_boot_epoch, offsetof(ayni_kernel_info_v1, boot_epoch) == 16);
AYNI_LAYOUT(info_durable_seq, offsetof(ayni_kernel_info_v1, durable_seq) == 24);
AYNI_LAYOUT(info_size, sizeof(ayni_kernel_info_v1) == 32);

AYNI_LAYOUT(report_struct_size, offsetof(ayni_step_report_v1, struct_size) == 0);
AYNI_LAYOUT(report_items_done, offsetof(ayni_step_report_v1, items_done) == 4);
AYNI_LAYOUT(report_more_ready, offsetof(ayni_step_report_v1, more_ready) == 8);
AYNI_LAYOUT(report_reserved, offsetof(ayni_step_report_v1, reserved) == 12);
AYNI_LAYOUT(report_next_deadline_us, offsetof(ayni_step_report_v1, next_deadline_us) == 16);
AYNI_LAYOUT(report_size, sizeof(ayni_step_report_v1) == 24);

AYNI_LAYOUT(string_data, offsetof(ayni_string_v1, data) == 0);
AYNI_LAYOUT(string_size, offsetof(ayni_string_v1, size) == AYNI_PTR);
AYNI_LAYOUT(string_struct_size, sizeof(ayni_string_v1) == 2 * AYNI_PTR);

AYNI_LAYOUT(ref_origin_device_id, offsetof(ayni_object_ref_v1, origin_device_id) == 0);
AYNI_LAYOUT(ref_id, offsetof(ayni_object_ref_v1, id) == 8);
AYNI_LAYOUT(ref_revision, offsetof(ayni_object_ref_v1, revision) == 16);
AYNI_LAYOUT(ref_kind, offsetof(ayni_object_ref_v1, kind) == 20);
AYNI_LAYOUT(ref_size, sizeof(ayni_object_ref_v1) == 24);

AYNI_LAYOUT(error_struct_size, offsetof(ayni_error_v1, struct_size) == 0);
AYNI_LAYOUT(error_status, offsetof(ayni_error_v1, status) == 4);
AYNI_LAYOUT(error_reason, offsetof(ayni_error_v1, reason) == 8);
AYNI_LAYOUT(error_reserved, offsetof(ayni_error_v1, reserved) == 12);
AYNI_LAYOUT(error_object, offsetof(ayni_error_v1, object) == 16);
AYNI_LAYOUT(error_size, sizeof(ayni_error_v1) == 40);

AYNI_LAYOUT(principal_struct_size, offsetof(ayni_principal_desc_v1, struct_size) == 0);
AYNI_LAYOUT(principal_kind, offsetof(ayni_principal_desc_v1, kind) == 4);
AYNI_LAYOUT(principal_id, offsetof(ayni_principal_desc_v1, principal_id) == 8);
AYNI_LAYOUT(principal_binding_revision, offsetof(ayni_principal_desc_v1, binding_revision) == 16);
AYNI_LAYOUT(principal_reserved, offsetof(ayni_principal_desc_v1, reserved) == 20);
AYNI_LAYOUT(principal_size, sizeof(ayni_principal_desc_v1) == 24);

AYNI_LAYOUT(session_struct_size, offsetof(ayni_session_desc_v1, struct_size) == 0);
AYNI_LAYOUT(session_channel, offsetof(ayni_session_desc_v1, channel) == 4);
AYNI_LAYOUT(session_principal_id, offsetof(ayni_session_desc_v1, principal_id) == 8);
AYNI_LAYOUT(session_size, sizeof(ayni_session_desc_v1) == 16);

AYNI_LAYOUT(agent_struct_size, offsetof(ayni_agent_desc_v1, struct_size) == 0);
AYNI_LAYOUT(agent_reserved, offsetof(ayni_agent_desc_v1, reserved) == 4);
AYNI_LAYOUT(agent_id, offsetof(ayni_agent_desc_v1, agent_id) == 8);
AYNI_LAYOUT(agent_size, sizeof(ayni_agent_desc_v1) == 16);

AYNI_LAYOUT(agent_info_struct_size, offsetof(ayni_agent_info_v1, struct_size) == 0);
AYNI_LAYOUT(agent_info_lifecycle, offsetof(ayni_agent_info_v1, lifecycle) == 4);
AYNI_LAYOUT(agent_info_agent_id, offsetof(ayni_agent_info_v1, agent_id) == 8);
AYNI_LAYOUT(agent_info_instance_id, offsetof(ayni_agent_info_v1, instance_id) == 16);
AYNI_LAYOUT(agent_info_commit_seq, offsetof(ayni_agent_info_v1, commit_seq) == 24);
AYNI_LAYOUT(agent_info_revision, offsetof(ayni_agent_info_v1, revision) == 32);
AYNI_LAYOUT(agent_info_reserved, offsetof(ayni_agent_info_v1, reserved) == 36);
AYNI_LAYOUT(agent_info_size, sizeof(ayni_agent_info_v1) == 40);

AYNI_LAYOUT(goal_type_struct_size, offsetof(ayni_goal_type_v1, struct_size) == 0);
AYNI_LAYOUT(goal_type_version, offsetof(ayni_goal_type_v1, version) == 4);
AYNI_LAYOUT(goal_type_id, offsetof(ayni_goal_type_v1, goal_type_id) == 8);
AYNI_LAYOUT(goal_type_ctx, offsetof(ayni_goal_type_v1, ctx) == 8 + 2 * AYNI_PTR);
AYNI_LAYOUT(goal_type_validate, offsetof(ayni_goal_type_v1, validate) == 8 + 3 * AYNI_PTR);
AYNI_LAYOUT(goal_type_trigger_kinds,
            offsetof(ayni_goal_type_v1, trigger_kinds) == 8 + 4 * AYNI_PTR);
AYNI_LAYOUT(goal_type_reserved, offsetof(ayni_goal_type_v1, reserved) == 12 + 4 * AYNI_PTR);
AYNI_LAYOUT(goal_type_size, sizeof(ayni_goal_type_v1) == 16 + 4 * AYNI_PTR);

AYNI_LAYOUT(goal_info_struct_size, offsetof(ayni_goal_info_v1, struct_size) == 0);
AYNI_LAYOUT(goal_info_lifecycle, offsetof(ayni_goal_info_v1, lifecycle) == 4);
AYNI_LAYOUT(goal_info_goal_id, offsetof(ayni_goal_info_v1, goal_id) == 8);
AYNI_LAYOUT(goal_info_parent_goal_id, offsetof(ayni_goal_info_v1, parent_goal_id) == 16);
AYNI_LAYOUT(goal_info_owner_instance_id, offsetof(ayni_goal_info_v1, owner_instance_id) == 24);
AYNI_LAYOUT(goal_info_origin_principal_id, offsetof(ayni_goal_info_v1, origin_principal_id) == 32);
AYNI_LAYOUT(goal_info_transition_id, offsetof(ayni_goal_info_v1, transition_id) == 40);
AYNI_LAYOUT(goal_info_commit_seq, offsetof(ayni_goal_info_v1, commit_seq) == 48);
AYNI_LAYOUT(goal_info_revision, offsetof(ayni_goal_info_v1, revision) == 56);
AYNI_LAYOUT(goal_info_cancel_revision, offsetof(ayni_goal_info_v1, cancel_revision) == 60);
AYNI_LAYOUT(goal_info_origin, offsetof(ayni_goal_info_v1, origin) == 64);
AYNI_LAYOUT(goal_info_origin_channel, offsetof(ayni_goal_info_v1, origin_channel) == 68);
AYNI_LAYOUT(goal_info_origin_binding_revision,
            offsetof(ayni_goal_info_v1, origin_binding_revision) == 72);
AYNI_LAYOUT(goal_info_depth, offsetof(ayni_goal_info_v1, depth) == 76);
AYNI_LAYOUT(goal_info_goal_type_version, offsetof(ayni_goal_info_v1, goal_type_version) == 80);
AYNI_LAYOUT(goal_info_goal_type_id_size, offsetof(ayni_goal_info_v1, goal_type_id_size) == 84);
AYNI_LAYOUT(goal_info_goal_type_id, offsetof(ayni_goal_info_v1, goal_type_id) == 88);
AYNI_LAYOUT(goal_info_trigger, offsetof(ayni_goal_info_v1, trigger) == 120);
AYNI_LAYOUT(goal_info_occurrences, offsetof(ayni_goal_info_v1, occurrences) == 124);
AYNI_LAYOUT(goal_info_last_task_id, offsetof(ayni_goal_info_v1, last_task_id) == 128);
AYNI_LAYOUT(goal_info_size, sizeof(ayni_goal_info_v1) == 136);

AYNI_LAYOUT(intent_struct_size, offsetof(ayni_intent_v1, struct_size) == 0);
AYNI_LAYOUT(intent_kind, offsetof(ayni_intent_v1, kind) == 4);
AYNI_LAYOUT(intent_command_id, offsetof(ayni_intent_v1, command_id) == 8);
AYNI_LAYOUT(intent_session_id, offsetof(ayni_intent_v1, session_id) == 16);
AYNI_LAYOUT(intent_expected_revision, offsetof(ayni_intent_v1, expected_revision) == 24);
AYNI_LAYOUT(intent_reserved, offsetof(ayni_intent_v1, reserved) == 28);
AYNI_LAYOUT(intent_body, offsetof(ayni_intent_v1, body) == 32);
AYNI_LAYOUT(intent_size, sizeof(ayni_intent_v1) == AYNI_ROUND8(32 + AYNI_PTR));

AYNI_LAYOUT(create_struct_size, offsetof(ayni_goal_create_v1, struct_size) == 0);
AYNI_LAYOUT(create_goal_type_version, offsetof(ayni_goal_create_v1, goal_type_version) == 4);
AYNI_LAYOUT(create_goal_type_id, offsetof(ayni_goal_create_v1, goal_type_id) == 8);
AYNI_LAYOUT(create_owner_instance_id,
            offsetof(ayni_goal_create_v1, owner_instance_id) == 8 + 2 * AYNI_PTR);
AYNI_LAYOUT(create_parent_goal_id,
            offsetof(ayni_goal_create_v1, parent_goal_id) == 16 + 2 * AYNI_PTR);
AYNI_LAYOUT(create_parameters, offsetof(ayni_goal_create_v1, parameters) == 24 + 2 * AYNI_PTR);
AYNI_LAYOUT(create_trigger, offsetof(ayni_goal_create_v1, trigger) == 24 + 4 * AYNI_PTR);
AYNI_LAYOUT(create_reserved, offsetof(ayni_goal_create_v1, reserved) == 28 + 4 * AYNI_PTR);
AYNI_LAYOUT(create_size, sizeof(ayni_goal_create_v1) == 32 + 4 * AYNI_PTR);

AYNI_LAYOUT(target_struct_size, offsetof(ayni_goal_target_v1, struct_size) == 0);
AYNI_LAYOUT(target_reserved, offsetof(ayni_goal_target_v1, reserved) == 4);
AYNI_LAYOUT(target_goal_id, offsetof(ayni_goal_target_v1, goal_id) == 8);
AYNI_LAYOUT(target_size, sizeof(ayni_goal_target_v1) == 16);

AYNI_LAYOUT(task_info_struct_size, offsetof(ayni_task_info_v1, struct_size) == 0);
AYNI_LAYOUT(task_info_lifecycle, offsetof(ayni_task_info_v1, lifecycle) == 4);
AYNI_LAYOUT(task_info_task_id, offsetof(ayni_task_info_v1, task_id) == 8);
AYNI_LAYOUT(task_info_goal_id, offsetof(ayni_task_info_v1, goal_id) == 16);
AYNI_LAYOUT(task_info_owner_instance_id, offsetof(ayni_task_info_v1, owner_instance_id) == 24);
AYNI_LAYOUT(task_info_transition_id, offsetof(ayni_task_info_v1, transition_id) == 32);
AYNI_LAYOUT(task_info_commit_seq, offsetof(ayni_task_info_v1, commit_seq) == 40);
AYNI_LAYOUT(task_info_revision, offsetof(ayni_task_info_v1, revision) == 48);
AYNI_LAYOUT(task_info_cancel_revision, offsetof(ayni_task_info_v1, cancel_revision) == 52);
AYNI_LAYOUT(task_info_occurrence, offsetof(ayni_task_info_v1, occurrence) == 56);
AYNI_LAYOUT(task_info_reserved, offsetof(ayni_task_info_v1, reserved) == 60);
AYNI_LAYOUT(task_info_size, sizeof(ayni_task_info_v1) == 64);

AYNI_LAYOUT(task_target_struct_size, offsetof(ayni_task_target_v1, struct_size) == 0);
AYNI_LAYOUT(task_target_reserved, offsetof(ayni_task_target_v1, reserved) == 4);
AYNI_LAYOUT(task_target_task_id, offsetof(ayni_task_target_v1, task_id) == 8);
AYNI_LAYOUT(task_target_size, sizeof(ayni_task_target_v1) == 16);

AYNI_LAYOUT(instance_target_struct_size, offsetof(ayni_instance_target_v1, struct_size) == 0);
AYNI_LAYOUT(instance_target_reserved, offsetof(ayni_instance_target_v1, reserved) == 4);
AYNI_LAYOUT(instance_target_instance_id, offsetof(ayni_instance_target_v1, instance_id) == 8);
AYNI_LAYOUT(instance_target_size, sizeof(ayni_instance_target_v1) == 16);

AYNI_LAYOUT(intent_result_struct_size, offsetof(ayni_intent_result_v1, struct_size) == 0);
AYNI_LAYOUT(intent_result_reserved, offsetof(ayni_intent_result_v1, reserved) == 4);
AYNI_LAYOUT(intent_result_object, offsetof(ayni_intent_result_v1, object) == 8);
AYNI_LAYOUT(intent_result_commit_seq, offsetof(ayni_intent_result_v1, commit_seq) == 32);
AYNI_LAYOUT(intent_result_size, sizeof(ayni_intent_result_v1) == 40);
