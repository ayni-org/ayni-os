/* ADR-0089: an independent C99 caller owns the arena and Host lifetime. */
#include <ayni/ayni.h>
#include <stdint.h>
#include <stdlib.h>

static uint64_t now(void *context) {
    (void)context;
    return 1;
}
static void signal(void *context) {
    (void)context;
}

int main(void) {
    ayni_config_v1 config = {0};
    ayni_host_v1 host = {0};
    ayni_kernel_t *kernel = NULL;
    size_t bytes = 0, alignment = 0;
    config.struct_size = sizeof(config);
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.ingress_capacity = 1;
    config.event_body_max_bytes = 1;
    config.completion_slot_count = 1;
    config.agent_capacity = 1;
    config.goal_type_capacity = 1;
    config.goal_capacity = 1;
    config.session_capacity = 1;
    config.goal_depth_max = 1;
    config.goal_children_max = 1;
    config.task_capacity = 1;
    config.strategy_capacity = 1;
    config.policy_intent_capacity = 1;
    config.task_step_capacity = 1;
    config.command_capacity = 1;
    config.capability_capacity = 1;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN;
    config.grant_capacity = 1;
    config.grant_delegation_depth_max = 1;
    config.goal_requirement_capacity = 1;
    host.struct_size = sizeof(host);
    host.monotonic_now_us = now;
    host.wake = signal;
    host.ingress_lock = signal;
    host.ingress_unlock = signal;
    if (ayni_kernel_memory_requirements(&config, &bytes, &alignment) != AYNI_OK) {
        return EXIT_FAILURE;
    }
    /* malloc aligns fundamental C++ types; Core's queried alignment is checked. */
    void *memory = malloc(bytes);
    if (memory == NULL || (uintptr_t)memory % alignment != 0) {
        free(memory);
        return EXIT_FAILURE;
    }
    const ayni_status_t created = ayni_create(&config, &host, memory, bytes, &kernel);
    const ayni_status_t closed = created == AYNI_OK ? ayni_destroy(kernel) : created;
    free(memory);
    return created == AYNI_OK && closed == AYNI_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
