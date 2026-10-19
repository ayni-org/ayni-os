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
AYNI_LAYOUT(config_size, sizeof(ayni_config_v1) == 24);

AYNI_LAYOUT(host_struct_size, offsetof(ayni_host_v1, struct_size) == 0);
AYNI_LAYOUT(host_ctx, offsetof(ayni_host_v1, ctx) == AYNI_PTR);
AYNI_LAYOUT(host_monotonic_now_us, offsetof(ayni_host_v1, monotonic_now_us) == 2 * AYNI_PTR);
AYNI_LAYOUT(host_wake, offsetof(ayni_host_v1, wake) == 3 * AYNI_PTR);
AYNI_LAYOUT(host_ingress_lock, offsetof(ayni_host_v1, ingress_lock) == 4 * AYNI_PTR);
AYNI_LAYOUT(host_ingress_unlock, offsetof(ayni_host_v1, ingress_unlock) == 5 * AYNI_PTR);
AYNI_LAYOUT(host_size, sizeof(ayni_host_v1) == 6 * AYNI_PTR);

AYNI_LAYOUT(attempt_boot_epoch, offsetof(ayni_attempt_ref_v1, boot_epoch) == 0);
AYNI_LAYOUT(attempt_request_id, offsetof(ayni_attempt_ref_v1, request_id) == 8);
AYNI_LAYOUT(attempt_size, sizeof(ayni_attempt_ref_v1) == 16);

AYNI_LAYOUT(result_struct_size, offsetof(ayni_completion_result_v1, struct_size) == 0);
AYNI_LAYOUT(result_outcome, offsetof(ayni_completion_result_v1, outcome) == 4);
AYNI_LAYOUT(result_attempt, offsetof(ayni_completion_result_v1, attempt) == 8);
AYNI_LAYOUT(result_value, offsetof(ayni_completion_result_v1, value) == 24);
AYNI_LAYOUT(result_size, sizeof(ayni_completion_result_v1) == 32);
