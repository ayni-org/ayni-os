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

AYNI_LAYOUT(function_pointer_size, sizeof(void (*)(void)) == AYNI_PTR);

AYNI_LAYOUT(config_struct_size, offsetof(ayni_config_v1, struct_size) == 0);
AYNI_LAYOUT(config_abi_major, offsetof(ayni_config_v1, abi_major) == 4);
AYNI_LAYOUT(config_abi_minor, offsetof(ayni_config_v1, abi_minor) == 8);
AYNI_LAYOUT(config_ingress_capacity, offsetof(ayni_config_v1, ingress_capacity) == 12);
AYNI_LAYOUT(config_event_body_max_bytes, offsetof(ayni_config_v1, event_body_max_bytes) == 16);
AYNI_LAYOUT(config_completion_slot_count, offsetof(ayni_config_v1, completion_slot_count) == 20);
AYNI_LAYOUT(config_storage_buffer_bytes, offsetof(ayni_config_v1, storage_buffer_bytes) == 24);
AYNI_LAYOUT(config_size, sizeof(ayni_config_v1) == 28);

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
AYNI_LAYOUT(info_size, sizeof(ayni_kernel_info_v1) == 24);

AYNI_LAYOUT(report_struct_size, offsetof(ayni_step_report_v1, struct_size) == 0);
AYNI_LAYOUT(report_items_done, offsetof(ayni_step_report_v1, items_done) == 4);
AYNI_LAYOUT(report_more_ready, offsetof(ayni_step_report_v1, more_ready) == 8);
AYNI_LAYOUT(report_reserved, offsetof(ayni_step_report_v1, reserved) == 12);
AYNI_LAYOUT(report_next_deadline_us, offsetof(ayni_step_report_v1, next_deadline_us) == 16);
AYNI_LAYOUT(report_size, sizeof(ayni_step_report_v1) == 24);
