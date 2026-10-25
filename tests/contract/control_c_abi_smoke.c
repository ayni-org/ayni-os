/* C-PERMIT-012/ADR-0049: gate creation/destruction links from C99 without Core. */
#include <ayni/control.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    size_t bytes = 0;
    size_t alignment = 0;
    if (ayni_control_memory_requirements(&bytes, &alignment) != AYNI_OK) {
        return 1;
    }
    void *memory = malloc(bytes);
    if (memory == NULL) {
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
    const ayni_status_t created = ayni_control_create(&config, 0, memory, bytes, &gate);
    const ayni_status_t destroyed = created == AYNI_OK ? ayni_control_destroy(gate) : created;
    free(memory);
    printf("control gate: %zu bytes, alignment %zu; permit: %zu bytes\n", bytes, alignment,
           sizeof(ayni_execution_permit_v1));
    return destroyed == AYNI_OK ? 0 : 1;
}
