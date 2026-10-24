/*
 * Pins the published layout of the implemented ABI structs (C-VER-002): a
 * field may be appended, never moved, retyped or removed. A violation fails
 * the build. Pointer-sized fields are measured in sizeof(void *) so that the
 * same checks fit 32-bit targets; only the native build compiles them so far.
 */
#include <ayni/authority.h>
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
AYNI_LAYOUT(config_strategy_capacity, offsetof(ayni_config_v1, strategy_capacity) == 60);
AYNI_LAYOUT(config_policy_intent_capacity, offsetof(ayni_config_v1, policy_intent_capacity) == 64);
AYNI_LAYOUT(config_task_step_capacity, offsetof(ayni_config_v1, task_step_capacity) == 68);
AYNI_LAYOUT(config_task_checkpoint_max_bytes,
            offsetof(ayni_config_v1, task_checkpoint_max_bytes) == 72);
AYNI_LAYOUT(config_command_capacity, offsetof(ayni_config_v1, command_capacity) == 76);
AYNI_LAYOUT(config_write_interval, offsetof(ayni_config_v1, storage_write_interval_us) == 80);
AYNI_LAYOUT(config_log_limit, offsetof(ayni_config_v1, storage_log_max_bytes) == 84);
AYNI_LAYOUT(config_capability_capacity, offsetof(ayni_config_v1, capability_capacity) == 88);
AYNI_LAYOUT(config_scope_max_bytes, offsetof(ayni_config_v1, authority_scope_max_bytes) == 92);
AYNI_LAYOUT(config_grant_capacity, offsetof(ayni_config_v1, grant_capacity) == 96);
AYNI_LAYOUT(config_grant_depth, offsetof(ayni_config_v1, grant_delegation_depth_max) == 100);
AYNI_LAYOUT(config_goal_requirements, offsetof(ayni_config_v1, goal_requirement_capacity) == 104);
AYNI_LAYOUT(config_size, sizeof(ayni_config_v1) == 108);

AYNI_LAYOUT(host_struct_size, offsetof(ayni_host_v1, struct_size) == 0);
AYNI_LAYOUT(host_ctx, offsetof(ayni_host_v1, ctx) == AYNI_PTR);
AYNI_LAYOUT(host_monotonic_now_us, offsetof(ayni_host_v1, monotonic_now_us) == 2 * AYNI_PTR);
AYNI_LAYOUT(host_wake, offsetof(ayni_host_v1, wake) == 3 * AYNI_PTR);
AYNI_LAYOUT(host_ingress_lock, offsetof(ayni_host_v1, ingress_lock) == 4 * AYNI_PTR);
AYNI_LAYOUT(host_ingress_unlock, offsetof(ayni_host_v1, ingress_unlock) == 5 * AYNI_PTR);
AYNI_LAYOUT(host_storage_append, offsetof(ayni_host_v1, storage_append) == 6 * AYNI_PTR);
AYNI_LAYOUT(host_storage_read_prefix, offsetof(ayni_host_v1, storage_read_prefix) == 7 * AYNI_PTR);
AYNI_LAYOUT(host_read_log, offsetof(ayni_host_v1, storage_read_log) == 8 * AYNI_PTR);
AYNI_LAYOUT(host_fence, offsetof(ayni_host_v1, storage_fence_writers) == 9 * AYNI_PTR);
AYNI_LAYOUT(host_write_snapshot, offsetof(ayni_host_v1, storage_write_snapshot) == 10 * AYNI_PTR);
AYNI_LAYOUT(host_read_snapshot, offsetof(ayni_host_v1, storage_read_snapshot) == 11 * AYNI_PTR);
AYNI_LAYOUT(host_trim, offsetof(ayni_host_v1, storage_trim_prefix) == 12 * AYNI_PTR);
AYNI_LAYOUT(host_free, offsetof(ayni_host_v1, storage_free_bytes) == 13 * AYNI_PTR);
AYNI_LAYOUT(host_size, sizeof(ayni_host_v1) == 14 * AYNI_PTR);

AYNI_LAYOUT(attempt_boot_epoch, offsetof(ayni_attempt_ref_v1, boot_epoch) == 0);
AYNI_LAYOUT(attempt_request_id, offsetof(ayni_attempt_ref_v1, request_id) == 8);
AYNI_LAYOUT(attempt_size, sizeof(ayni_attempt_ref_v1) == 16);

AYNI_LAYOUT(result_struct_size, offsetof(ayni_completion_result_v1, struct_size) == 0);
AYNI_LAYOUT(result_outcome, offsetof(ayni_completion_result_v1, outcome) == 4);
AYNI_LAYOUT(result_attempt, offsetof(ayni_completion_result_v1, attempt) == 8);
AYNI_LAYOUT(result_value, offsetof(ayni_completion_result_v1, value) == 24);
AYNI_LAYOUT(result_cursor, offsetof(ayni_completion_result_v1, next_cursor) == 32);
AYNI_LAYOUT(result_size, sizeof(ayni_completion_result_v1) == 40);

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
AYNI_LAYOUT(info_events_unmatched, offsetof(ayni_kernel_info_v1, events_unmatched) == 32);
AYNI_LAYOUT(info_events_discarded, offsetof(ayni_kernel_info_v1, events_discarded) == 40);
AYNI_LAYOUT(info_size, sizeof(ayni_kernel_info_v1) == 48);

AYNI_LAYOUT(report_struct_size, offsetof(ayni_step_report_v1, struct_size) == 0);
AYNI_LAYOUT(report_items_done, offsetof(ayni_step_report_v1, items_done) == 4);
AYNI_LAYOUT(report_more_ready, offsetof(ayni_step_report_v1, more_ready) == 8);
AYNI_LAYOUT(report_reserved, offsetof(ayni_step_report_v1, reserved) == 12);
AYNI_LAYOUT(report_next_deadline_us, offsetof(ayni_step_report_v1, next_deadline_us) == 16);
AYNI_LAYOUT(report_durable_seq, offsetof(ayni_step_report_v1, durable_seq) == 24);
AYNI_LAYOUT(report_size, sizeof(ayni_step_report_v1) == 32);

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
AYNI_LAYOUT(session_binding_revision, offsetof(ayni_session_desc_v1, binding_revision) == 16);
AYNI_LAYOUT(session_reserved, offsetof(ayni_session_desc_v1, reserved) == 20);
AYNI_LAYOUT(session_size, sizeof(ayni_session_desc_v1) == 24);

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
AYNI_LAYOUT(agent_info_fault_reason, offsetof(ayni_agent_info_v1, fault_reason) == 36);
AYNI_LAYOUT(agent_info_size, sizeof(ayni_agent_info_v1) == 40);

AYNI_LAYOUT(goal_type_struct_size, offsetof(ayni_goal_type_v1, struct_size) == 0);
AYNI_LAYOUT(goal_type_version, offsetof(ayni_goal_type_v1, version) == 4);
AYNI_LAYOUT(goal_type_id, offsetof(ayni_goal_type_v1, goal_type_id) == 8);
AYNI_LAYOUT(goal_type_ctx, offsetof(ayni_goal_type_v1, ctx) == 8 + 2 * AYNI_PTR);
AYNI_LAYOUT(goal_type_validate, offsetof(ayni_goal_type_v1, validate) == 8 + 3 * AYNI_PTR);
AYNI_LAYOUT(goal_type_trigger_kinds,
            offsetof(ayni_goal_type_v1, trigger_kinds) == 8 + 4 * AYNI_PTR);
AYNI_LAYOUT(goal_type_reserved, offsetof(ayni_goal_type_v1, reserved) == 12 + 4 * AYNI_PTR);
AYNI_LAYOUT(goal_type_occurrence_wait_max_us,
            offsetof(ayni_goal_type_v1, occurrence_wait_max_us) == 16 + 4 * AYNI_PTR);
AYNI_LAYOUT(goal_type_strategy_id, offsetof(ayni_goal_type_v1, strategy_id) == 24 + 4 * AYNI_PTR);
AYNI_LAYOUT(goal_type_export_requirements,
            offsetof(ayni_goal_type_v1, export_requirements) == 24 + 6 * AYNI_PTR);
AYNI_LAYOUT(goal_type_size, sizeof(ayni_goal_type_v1) == 24 + 7 * AYNI_PTR);

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
AYNI_LAYOUT(goal_info_trigger_delay_us, offsetof(ayni_goal_info_v1, trigger_delay_us) == 136);
AYNI_LAYOUT(goal_info_trigger_period_us, offsetof(ayni_goal_info_v1, trigger_period_us) == 144);
AYNI_LAYOUT(goal_info_next_due_us, offsetof(ayni_goal_info_v1, next_due_us) == 152);
AYNI_LAYOUT(goal_info_expires_us, offsetof(ayni_goal_info_v1, expires_us) == 160);
AYNI_LAYOUT(goal_info_occurrences_skipped, offsetof(ayni_goal_info_v1, occurrences_skipped) == 168);
AYNI_LAYOUT(goal_info_skip_reason, offsetof(ayni_goal_info_v1, skip_reason) == 172);
AYNI_LAYOUT(goal_info_wait_reason, offsetof(ayni_goal_info_v1, wait_reason) == 176);
AYNI_LAYOUT(goal_info_hold_reason, offsetof(ayni_goal_info_v1, hold_reason) == 180);
AYNI_LAYOUT(goal_info_trigger_source_size, offsetof(ayni_goal_info_v1, trigger_source_size) == 184);
AYNI_LAYOUT(goal_info_trigger_source, offsetof(ayni_goal_info_v1, trigger_source) == 188);
AYNI_LAYOUT(goal_info_original_expiry, offsetof(ayni_goal_info_v1, original_expiry_us) == 224);
AYNI_LAYOUT(goal_info_size, sizeof(ayni_goal_info_v1) == 232);

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
AYNI_LAYOUT(create_trigger_delay_us,
            offsetof(ayni_goal_create_v1, trigger_delay_us) == 32 + 4 * AYNI_PTR);
AYNI_LAYOUT(create_trigger_period_us,
            offsetof(ayni_goal_create_v1, trigger_period_us) == 40 + 4 * AYNI_PTR);
AYNI_LAYOUT(create_expiry_us, offsetof(ayni_goal_create_v1, expiry_us) == 48 + 4 * AYNI_PTR);
AYNI_LAYOUT(create_trigger_source,
            offsetof(ayni_goal_create_v1, trigger_source) == 56 + 4 * AYNI_PTR);
AYNI_LAYOUT(create_size, sizeof(ayni_goal_create_v1) == AYNI_ROUND8(56 + 6 * AYNI_PTR));

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
AYNI_LAYOUT(task_info_occurrence_count, offsetof(ayni_task_info_v1, occurrence_count) == 60);
AYNI_LAYOUT(task_info_due_us, offsetof(ayni_task_info_v1, due_us) == 64);
AYNI_LAYOUT(task_info_hold_status, offsetof(ayni_task_info_v1, hold_status) == 72);
AYNI_LAYOUT(task_info_hold_reason, offsetof(ayni_task_info_v1, hold_reason) == 76);
AYNI_LAYOUT(task_info_step_count, offsetof(ayni_task_info_v1, step_count) == 80);
AYNI_LAYOUT(task_info_reserved, offsetof(ayni_task_info_v1, reserved) == 84);
AYNI_LAYOUT(task_info_size, sizeof(ayni_task_info_v1) == 88);

AYNI_LAYOUT(private_checkpoint_strategy_id, offsetof(ayni_private_checkpoint_v1, strategy_id) == 0);
AYNI_LAYOUT(private_checkpoint_strategy_version,
            offsetof(ayni_private_checkpoint_v1, strategy_version) == 2 * AYNI_PTR);
AYNI_LAYOUT(private_checkpoint_reserved,
            offsetof(ayni_private_checkpoint_v1, reserved) == 4 + 2 * AYNI_PTR);
AYNI_LAYOUT(private_checkpoint_state,
            offsetof(ayni_private_checkpoint_v1, state) == 8 + 2 * AYNI_PTR);
AYNI_LAYOUT(private_checkpoint_size, sizeof(ayni_private_checkpoint_v1) == 8 + 4 * AYNI_PTR);

AYNI_LAYOUT(task_step_step_key, offsetof(ayni_task_step_v1, step_key) == 0);
AYNI_LAYOUT(task_step_size, sizeof(ayni_task_step_v1) == 2 * AYNI_PTR);

AYNI_LAYOUT(snapshot_struct_size, offsetof(ayni_task_snapshot_v1, struct_size) == 0);
AYNI_LAYOUT(snapshot_revision, offsetof(ayni_task_snapshot_v1, revision) == 4);
AYNI_LAYOUT(snapshot_task_id, offsetof(ayni_task_snapshot_v1, task_id) == 8);
AYNI_LAYOUT(snapshot_goal_id, offsetof(ayni_task_snapshot_v1, goal_id) == 16);
AYNI_LAYOUT(snapshot_owner_instance_id, offsetof(ayni_task_snapshot_v1, owner_instance_id) == 24);
AYNI_LAYOUT(snapshot_due_us, offsetof(ayni_task_snapshot_v1, due_us) == 32);
AYNI_LAYOUT(snapshot_now_us, offsetof(ayni_task_snapshot_v1, now_us) == 40);
AYNI_LAYOUT(snapshot_occurrence, offsetof(ayni_task_snapshot_v1, occurrence) == 48);
AYNI_LAYOUT(snapshot_occurrence_count, offsetof(ayni_task_snapshot_v1, occurrence_count) == 52);
AYNI_LAYOUT(snapshot_goal_type_version, offsetof(ayni_task_snapshot_v1, goal_type_version) == 56);
AYNI_LAYOUT(snapshot_reserved, offsetof(ayni_task_snapshot_v1, reserved) == 60);
AYNI_LAYOUT(snapshot_goal_type_id, offsetof(ayni_task_snapshot_v1, goal_type_id) == 64);
AYNI_LAYOUT(snapshot_goal_parameters,
            offsetof(ayni_task_snapshot_v1, goal_parameters) == 64 + 2 * AYNI_PTR);
AYNI_LAYOUT(snapshot_checkpoint, offsetof(ayni_task_snapshot_v1, checkpoint) == 64 + 4 * AYNI_PTR);
AYNI_LAYOUT(snapshot_step_count, offsetof(ayni_task_snapshot_v1, step_count) == 72 + 8 * AYNI_PTR);
AYNI_LAYOUT(snapshot_step_capacity,
            offsetof(ayni_task_snapshot_v1, step_capacity) == 76 + 8 * AYNI_PTR);
AYNI_LAYOUT(snapshot_steps, offsetof(ayni_task_snapshot_v1, steps) == 80 + 8 * AYNI_PTR);
AYNI_LAYOUT(snapshot_size, sizeof(ayni_task_snapshot_v1) == AYNI_ROUND8(80 + 9 * AYNI_PTR));

AYNI_LAYOUT(policy_intent_struct_size, offsetof(ayni_policy_intent_v1, struct_size) == 0);
AYNI_LAYOUT(policy_intent_kind, offsetof(ayni_policy_intent_v1, kind) == 4);
AYNI_LAYOUT(policy_intent_expected_revision,
            offsetof(ayni_policy_intent_v1, expected_revision) == 8);
AYNI_LAYOUT(policy_intent_step_key_size, offsetof(ayni_policy_intent_v1, step_key_size) == 12);
AYNI_LAYOUT(policy_intent_step_key, offsetof(ayni_policy_intent_v1, step_key) == 16);
AYNI_LAYOUT(policy_intent_size, sizeof(ayni_policy_intent_v1) == 16 + AYNI_CATALOG_ID_MAX_BYTES);

AYNI_LAYOUT(policy_output_struct_size, offsetof(ayni_policy_output_v1, struct_size) == 0);
AYNI_LAYOUT(policy_output_intent_capacity, offsetof(ayni_policy_output_v1, intent_capacity) == 4);
AYNI_LAYOUT(policy_output_intent_count, offsetof(ayni_policy_output_v1, intent_count) == 8);
AYNI_LAYOUT(policy_output_reserved, offsetof(ayni_policy_output_v1, reserved) == 12);
AYNI_LAYOUT(policy_output_intents, offsetof(ayni_policy_output_v1, intents) == 16);
AYNI_LAYOUT(policy_output_checkpoint_capacity,
            offsetof(ayni_policy_output_v1, checkpoint_capacity) == 16 + AYNI_PTR);
AYNI_LAYOUT(policy_output_checkpoint_size,
            offsetof(ayni_policy_output_v1, checkpoint_size) == 20 + AYNI_PTR);
AYNI_LAYOUT(policy_output_checkpoint, offsetof(ayni_policy_output_v1, checkpoint) == 24 + AYNI_PTR);
AYNI_LAYOUT(policy_output_size, sizeof(ayni_policy_output_v1) == 24 + 2 * AYNI_PTR);

AYNI_LAYOUT(strategy_struct_size, offsetof(ayni_strategy_v1, struct_size) == 0);
AYNI_LAYOUT(strategy_version, offsetof(ayni_strategy_v1, version) == 4);
AYNI_LAYOUT(strategy_id, offsetof(ayni_strategy_v1, strategy_id) == 8);
AYNI_LAYOUT(strategy_ctx, offsetof(ayni_strategy_v1, ctx) == 8 + 2 * AYNI_PTR);
AYNI_LAYOUT(strategy_decide, offsetof(ayni_strategy_v1, decide) == 8 + 3 * AYNI_PTR);
AYNI_LAYOUT(strategy_max_intents_per_call,
            offsetof(ayni_strategy_v1, max_intents_per_call) == 8 + 4 * AYNI_PTR);
AYNI_LAYOUT(strategy_intent_size, offsetof(ayni_strategy_v1, intent_size) == 12 + 4 * AYNI_PTR);
AYNI_LAYOUT(strategy_max_checkpoint_bytes,
            offsetof(ayni_strategy_v1, max_checkpoint_bytes) == 16 + 4 * AYNI_PTR);
AYNI_LAYOUT(strategy_reserved, offsetof(ayni_strategy_v1, reserved) == 20 + 4 * AYNI_PTR);
AYNI_LAYOUT(strategy_size, sizeof(ayni_strategy_v1) == 24 + 4 * AYNI_PTR);

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
AYNI_LAYOUT(intent_result_size, sizeof(ayni_intent_result_v1) == 56);

AYNI_LAYOUT(event_struct_size, offsetof(ayni_event_v1, struct_size) == 0);
AYNI_LAYOUT(event_kind, offsetof(ayni_event_v1, kind) == 4);
AYNI_LAYOUT(event_source, offsetof(ayni_event_v1, source) == 8);
AYNI_LAYOUT(event_size, sizeof(ayni_event_v1) == 8 + 2 * AYNI_PTR);

/* Since 0.17: typed authorization inputs retain C99 pointer-size layouts. */
AYNI_LAYOUT(authority_integer_size, sizeof(ayni_authority_i64_range_v1) == 16);
AYNI_LAYOUT(authority_tokens_count,
            offsetof(ayni_authority_token_set_v1, member_count) == AYNI_PTR);
AYNI_LAYOUT(authority_tokens_size, sizeof(ayni_authority_token_set_v1) == AYNI_PTR + 8);
AYNI_LAYOUT(authority_values_size, sizeof(ayni_authority_values_v1) == 16);
AYNI_LAYOUT(authority_value_unit, offsetof(ayni_authority_value_set_v1, unit) == 8);
AYNI_LAYOUT(authority_value_values,
            offsetof(ayni_authority_value_set_v1, values) == 8 + 2 * AYNI_PTR);
AYNI_LAYOUT(authority_value_size, sizeof(ayni_authority_value_set_v1) == 24 + 2 * AYNI_PTR);
AYNI_LAYOUT(authority_field_domain, offsetof(ayni_authority_field_desc_v1, domain) == 16);
AYNI_LAYOUT(authority_field_size, sizeof(ayni_authority_field_desc_v1) == 40 + 2 * AYNI_PTR);
AYNI_LAYOUT(capability_id, offsetof(ayni_capability_desc_v1, capability_id) == 8);
AYNI_LAYOUT(capability_fields, offsetof(ayni_capability_desc_v1, fields) == 16 + 2 * AYNI_PTR);
AYNI_LAYOUT(capability_size, sizeof(ayni_capability_desc_v1) == 16 + 3 * AYNI_PTR);
AYNI_LAYOUT(scope_field_allowed, offsetof(ayni_scope_field_v1, allowed) == 8);
AYNI_LAYOUT(scope_field_size, sizeof(ayni_scope_field_v1) == 32 + 2 * AYNI_PTR);
AYNI_LAYOUT(scope_fields, offsetof(ayni_scope_v1, fields) == 8);
AYNI_LAYOUT(scope_size, sizeof(ayni_scope_v1) == 8 + AYNI_PTR);

/* Since 0.18: management inputs borrow views; diagnostic outputs own bytes. */
AYNI_LAYOUT(grant_create_struct_size, offsetof(ayni_grant_create_v1, struct_size) == 0);
AYNI_LAYOUT(grant_create_capability_version,
            offsetof(ayni_grant_create_v1, capability_version) == 4);
AYNI_LAYOUT(grant_create_capability_id, offsetof(ayni_grant_create_v1, capability_id) == 8);
AYNI_LAYOUT(grant_create_subject,
            offsetof(ayni_grant_create_v1, subject_instance_id) == 8 + 2 * AYNI_PTR);
AYNI_LAYOUT(grant_create_parent,
            offsetof(ayni_grant_create_v1, parent_grant_id) == 16 + 2 * AYNI_PTR);
AYNI_LAYOUT(grant_create_lifetime, offsetof(ayni_grant_create_v1, lifetime) == 24 + 2 * AYNI_PTR);
AYNI_LAYOUT(grant_create_flags, offsetof(ayni_grant_create_v1, flags) == 28 + 2 * AYNI_PTR);
AYNI_LAYOUT(grant_create_duration,
            offsetof(ayni_grant_create_v1, duration_us) == 32 + 2 * AYNI_PTR);
AYNI_LAYOUT(grant_create_scope, offsetof(ayni_grant_create_v1, scope) == 40 + 2 * AYNI_PTR);
AYNI_LAYOUT(grant_create_size, sizeof(ayni_grant_create_v1) == AYNI_ROUND8(48 + 3 * AYNI_PTR));

AYNI_LAYOUT(grant_info_struct_size, offsetof(ayni_grant_info_v1, struct_size) == 0);
AYNI_LAYOUT(grant_info_lifecycle, offsetof(ayni_grant_info_v1, lifecycle) == 4);
AYNI_LAYOUT(grant_info_id, offsetof(ayni_grant_info_v1, grant_id) == 8);
AYNI_LAYOUT(grant_info_subject, offsetof(ayni_grant_info_v1, subject_instance_id) == 16);
AYNI_LAYOUT(grant_info_parent, offsetof(ayni_grant_info_v1, parent_grant_id) == 24);
AYNI_LAYOUT(grant_info_owner, offsetof(ayni_grant_info_v1, owner_principal_id) == 32);
AYNI_LAYOUT(grant_info_owner_revision, offsetof(ayni_grant_info_v1, owner_binding_revision) == 40);
AYNI_LAYOUT(grant_info_capability_version, offsetof(ayni_grant_info_v1, capability_version) == 44);
AYNI_LAYOUT(grant_info_capability_id, offsetof(ayni_grant_info_v1, capability_id) == 48);
AYNI_LAYOUT(grant_info_capability_id_size, offsetof(ayni_grant_info_v1, capability_id_size) == 80);
AYNI_LAYOUT(grant_info_field_count, offsetof(ayni_grant_info_v1, field_count) == 84);
AYNI_LAYOUT(grant_info_allow_delegation, offsetof(ayni_grant_info_v1, allow_delegation) == 88);
AYNI_LAYOUT(grant_info_depth, offsetof(ayni_grant_info_v1, depth) == 92);
AYNI_LAYOUT(grant_info_lifetime, offsetof(ayni_grant_info_v1, lifetime) == 96);
AYNI_LAYOUT(grant_info_revision, offsetof(ayni_grant_info_v1, revision) == 100);
AYNI_LAYOUT(grant_info_duration, offsetof(ayni_grant_info_v1, duration_us) == 104);
AYNI_LAYOUT(grant_info_deadline, offsetof(ayni_grant_info_v1, deadline_us) == 112);
AYNI_LAYOUT(grant_info_created_transition,
            offsetof(ayni_grant_info_v1, created_transition_id) == 120);
AYNI_LAYOUT(grant_info_created_seq, offsetof(ayni_grant_info_v1, created_commit_seq) == 128);
AYNI_LAYOUT(grant_info_transition, offsetof(ayni_grant_info_v1, transition_id) == 136);
AYNI_LAYOUT(grant_info_seq, offsetof(ayni_grant_info_v1, commit_seq) == 144);
AYNI_LAYOUT(grant_info_effective_reason, offsetof(ayni_grant_info_v1, effective_reason) == 152);
AYNI_LAYOUT(grant_info_reserved, offsetof(ayni_grant_info_v1, reserved) == 156);
AYNI_LAYOUT(grant_info_goal, offsetof(ayni_grant_info_v1, bound_goal_id) == 200);
AYNI_LAYOUT(grant_info_size, sizeof(ayni_grant_info_v1) == 208);

AYNI_LAYOUT(grant_tokens_members, offsetof(ayni_grant_tokens_info_v1, members) == 0);
AYNI_LAYOUT(grant_tokens_sizes, offsetof(ayni_grant_tokens_info_v1, member_sizes) == 256);
AYNI_LAYOUT(grant_tokens_count, offsetof(ayni_grant_tokens_info_v1, member_count) == 288);
AYNI_LAYOUT(grant_tokens_reserved, offsetof(ayni_grant_tokens_info_v1, reserved) == 292);
AYNI_LAYOUT(grant_tokens_size, sizeof(ayni_grant_tokens_info_v1) == 296);
AYNI_LAYOUT(grant_field_values_size, sizeof(ayni_grant_field_values_v1) == 296);
AYNI_LAYOUT(grant_field_struct_size, offsetof(ayni_grant_field_info_v1, struct_size) == 0);
AYNI_LAYOUT(grant_field_id, offsetof(ayni_grant_field_info_v1, field_id) == 4);
AYNI_LAYOUT(grant_field_kind, offsetof(ayni_grant_field_info_v1, kind) == 8);
AYNI_LAYOUT(grant_field_usage, offsetof(ayni_grant_field_info_v1, usage) == 12);
AYNI_LAYOUT(grant_field_unit, offsetof(ayni_grant_field_info_v1, unit) == 16);
AYNI_LAYOUT(grant_field_unit_size, offsetof(ayni_grant_field_info_v1, unit_size) == 48);
AYNI_LAYOUT(grant_field_reserved, offsetof(ayni_grant_field_info_v1, reserved) == 52);
AYNI_LAYOUT(grant_field_values, offsetof(ayni_grant_field_info_v1, values) == 56);
AYNI_LAYOUT(grant_field_info_size, sizeof(ayni_grant_field_info_v1) == 352);

AYNI_LAYOUT(intent_result_phase, offsetof(ayni_intent_result_v1, phase) == 40);
AYNI_LAYOUT(intent_result_effect, offsetof(ayni_intent_result_v1, effect) == 44);
AYNI_LAYOUT(intent_result_boot, offsetof(ayni_intent_result_v1, applied_boot_epoch) == 48);
AYNI_LAYOUT(receipt_size, sizeof(ayni_intent_receipt_v1) == 72);
AYNI_LAYOUT(receipt_phase, offsetof(ayni_intent_receipt_v1, phase) == 4);
AYNI_LAYOUT(receipt_effect, offsetof(ayni_intent_receipt_v1, effect) == 8);
AYNI_LAYOUT(receipt_reserved, offsetof(ayni_intent_receipt_v1, reserved) == 12);
AYNI_LAYOUT(receipt_principal, offsetof(ayni_intent_receipt_v1, principal_id) == 16);
AYNI_LAYOUT(receipt_command, offsetof(ayni_intent_receipt_v1, command_id) == 24);
AYNI_LAYOUT(receipt_object, offsetof(ayni_intent_receipt_v1, object) == 32);
AYNI_LAYOUT(receipt_seq, offsetof(ayni_intent_receipt_v1, commit_seq) == 56);
AYNI_LAYOUT(receipt_boot, offsetof(ayni_intent_receipt_v1, applied_boot_epoch) == 64);
AYNI_LAYOUT(revoke_size, sizeof(ayni_grant_revoke_v1) == 16);
AYNI_LAYOUT(revoke_target, offsetof(ayni_grant_revoke_v1, grant_id) == 8);
AYNI_LAYOUT(grant_info_revoke_root, offsetof(ayni_grant_info_v1, revoked_by_root_id) == 160);
AYNI_LAYOUT(grant_info_cleanup_after, offsetof(ayni_grant_info_v1, cleanup_after_grant_id) == 168);
AYNI_LAYOUT(grant_info_cleanup_through,
            offsetof(ayni_grant_info_v1, cleanup_through_grant_id) == 176);
AYNI_LAYOUT(grant_info_cleanup_transition,
            offsetof(ayni_grant_info_v1, cleanup_transition_id) == 184);
AYNI_LAYOUT(grant_info_cleanup_seq, offsetof(ayni_grant_info_v1, cleanup_commit_seq) == 192);

/* ADR-0037: requirement callbacks and copied query outputs, ABI 0.21. */
AYNI_LAYOUT(requirement_input_size, sizeof(ayni_goal_requirement_v1) == 16 + 3 * AYNI_PTR);
AYNI_LAYOUT(requirement_sink_ctx, offsetof(ayni_goal_requirement_sink_v1, ctx) == AYNI_PTR);
AYNI_LAYOUT(requirement_sink_emit, offsetof(ayni_goal_requirement_sink_v1, emit) == 2 * AYNI_PTR);
AYNI_LAYOUT(goal_authority_info_size, sizeof(ayni_goal_authority_info_v1) == 64);
AYNI_LAYOUT(goal_requirement_info_size, sizeof(ayni_goal_requirement_info_v1) == 64);

/* ADR-0037: explicit complete approval request, ABI 0.23. */
AYNI_LAYOUT(goal_approve_struct, offsetof(ayni_goal_approve_v1, struct_size) == 0);
AYNI_LAYOUT(goal_approve_mask, offsetof(ayni_goal_approve_v1, missing_mask) == 4);
AYNI_LAYOUT(goal_approve_goal, offsetof(ayni_goal_approve_v1, goal_id) == 8);
AYNI_LAYOUT(goal_approve_lifetime, offsetof(ayni_goal_approve_v1, lifetime) == 16);
AYNI_LAYOUT(goal_approve_reserved, offsetof(ayni_goal_approve_v1, reserved) == 20);
AYNI_LAYOUT(goal_approve_duration, offsetof(ayni_goal_approve_v1, duration_us) == 24);
AYNI_LAYOUT(goal_approve_original, offsetof(ayni_goal_approve_v1, original) == 32);
AYNI_LAYOUT(goal_approve_size, sizeof(ayni_goal_approve_v1) == 32 + sizeof(ayni_goal_create_v1));

/* ADR-0038: copied Task admission facts and binding sequences, ABI 0.24. */
AYNI_LAYOUT(task_authority_info_size, sizeof(ayni_task_authority_info_v1) == 56);
AYNI_LAYOUT(task_authority_ref_size, sizeof(ayni_task_authority_reference_v1) == 24);
AYNI_LAYOUT(goal_binding_seq, offsetof(ayni_goal_authority_info_v1, binding_commit_seq) == 48);
AYNI_LAYOUT(goal_parent_binding_seq,
            offsetof(ayni_goal_authority_info_v1, parent_binding_commit_seq) == 56);

/* ADR-0038: full original echo and distinct authorization CAS, ABI 0.26. */
AYNI_LAYOUT(goal_reauthorize_struct, offsetof(ayni_goal_reauthorize_v1, struct_size) == 0);
AYNI_LAYOUT(goal_reauthorize_reserved, offsetof(ayni_goal_reauthorize_v1, reserved) == 4);
AYNI_LAYOUT(goal_reauthorize_goal, offsetof(ayni_goal_reauthorize_v1, goal_id) == 8);
AYNI_LAYOUT(goal_reauthorize_authority_cas,
            offsetof(ayni_goal_reauthorize_v1, expected_authority_commit_seq) == 16);
AYNI_LAYOUT(goal_reauthorize_original, offsetof(ayni_goal_reauthorize_v1, original) == 24);
AYNI_LAYOUT(goal_reauthorize_size,
            sizeof(ayni_goal_reauthorize_v1) == 24 + sizeof(ayni_goal_create_v1));
