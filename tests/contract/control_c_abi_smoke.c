/* C-PERMIT-012/ADR-0049/ADR-0112: gate and ControlSession link from C99 without Core. */
#include <ayni/control.h>
#include <ayni/control_loop.h>
#include <ayni/control_session.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    size_t bytes = 0;
    size_t alignment = 0;
    size_t session_bytes = 0;
    size_t session_alignment = 0;
    if (ayni_control_memory_requirements(&bytes, &alignment) != AYNI_OK ||
        ayni_control_session_memory_requirements(&session_bytes, &session_alignment) != AYNI_OK) {
        return 1;
    }
    void *memory = malloc(bytes);
    void *session_memory = malloc(session_bytes);
    if (memory == NULL || session_memory == NULL) {
        free(session_memory);
        free(memory);
        return 1;
    }
    ayni_control_config_v1 config = {0};
    config.struct_size = sizeof(config);
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.control_mode = 1;
    config.control_domain_id = 1;
    config.epochs = (ayni_permit_epochs_v1){1, 1, 1, 1, 1, 0};
    config.procedure_max_us = 1000;
    config.session_lease_max_us = 100;
    config.command_age_max_us = 10;
    config.limits.count = 1;
    config.limits.fields[0].field_id = (ayni_catalog_name_v1){1, "x"};
    config.limits.fields[0].unit = (ayni_catalog_name_v1){1, "u"};
    config.limits.fields[0].maximum = 1;
    ayni_control_gate_t *gate = NULL;
    ayni_control_session_t *session = NULL;
    ayni_control_session_info_v1 info = {0};
    info.struct_size = sizeof(info);
    const ayni_status_t created = ayni_control_create(&config, 0, memory, bytes, &gate);
    const ayni_status_t session_created =
        ayni_control_session_create(session_memory, session_bytes, &session);
    const ayni_status_t session_info = session_created == AYNI_OK
                                           ? ayni_control_session_get_info(session, &info)
                                           : session_created;
    const ayni_status_t session_destroyed =
        session_created == AYNI_OK ? ayni_control_session_destroy(session) : session_created;
    const ayni_status_t destroyed = created == AYNI_OK ? ayni_control_destroy(gate) : created;
    free(session_memory);
    free(memory);
    printf("control gate: %zu bytes, alignment %zu; session: %zu bytes, alignment %zu; permit: %zu "
           "bytes\n",
           bytes, alignment, session_bytes, session_alignment, sizeof(ayni_execution_permit_v1));
    ayni_control_loop_t loop = {0};
    loop.struct_size = sizeof(loop);
    loop.field_count = 1;
    loop.fields[0] = config.limits.fields[0].field_id;
    loop.max_cycle_gap_us = 100;
    ayni_control_segment_v1 desired = {0}, effective = {0};
    desired.struct_size = sizeof(desired);
    desired.field_count = 1;
    desired.fields[0] =
        (ayni_control_value_v1){config.limits.fields[0].field_id, config.limits.fields[0].unit, 1};
    ayni_control_loop_action_t action = 0;
    const ayni_status_t limited =
        ayni_control_loop_limit(&loop, &desired, NULL, 0, 0, 100, &effective, &action, &loop);
    return (limited == AYNI_OK && action == AYNI_LOOP_APPLY && effective.fields[0].value == 1 &&
            destroyed == AYNI_OK && session_info == AYNI_OK &&
            info.state == AYNI_CONTROL_SESSION_IDLE && session_destroyed == AYNI_OK)
               ? 0
               : 1;
}
