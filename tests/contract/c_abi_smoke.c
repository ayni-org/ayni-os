/* Uses the implemented ABI from strict C99 with caller-provided memory. */

#include <ayni/ayni.h>

#include <stdio.h>

static uint64_t test_clock(void *ctx) {
    (void)ctx;
    return 1u;
}

/* Single-threaded, so wake and the ingress lock have nothing to do. */
static void test_wake(void *ctx) {
    (void)ctx;
}
static void test_lock(void *ctx) {
    (void)ctx;
}
static void test_unlock(void *ctx) {
    (void)ctx;
}

/* C99 has no alignas; a union of the widest scalar types gives the strictest
 * fundamental alignment, which the reported requirement must not exceed. */
typedef union {
    long double ld;
    uint64_t u64;
    void *ptr;
    void (*fn)(void);
} max_aligned_t;

static max_aligned_t kernel_memory[512];

static int expect(int condition, const char *what) {
    if (!condition) {
        fprintf(stderr, "c_abi_smoke: %s\n", what);
    }
    return condition ? 0 : 1;
}

static ayni_status_t accept_parameters(void *ctx, ayni_bytes_v1 parameters) {
    (void)ctx;
    (void)parameters;
    return AYNI_OK;
}

/* Registration works in CONFIGURING; sessions, intents and queries wait for
 * RUNNING, which this storage-less kernel never reaches. */
static int check_work_model(ayni_kernel_t *kernel) {
    ayni_principal_desc_v1 owner = {0};
    ayni_agent_desc_v1 agent = {0};
    ayni_goal_type_v1 goal_type = {0};
    ayni_session_desc_v1 session = {0};
    ayni_goal_create_v1 create = {0};
    ayni_intent_v1 intent = {0};
    ayni_intent_result_v1 result = {0};
    ayni_error_v1 error = {0};
    ayni_agent_info_v1 agent_info = {0};
    ayni_goal_info_v1 goal_info = {0};
    uint64_t session_id = 0;
    int failures = 0;

    owner.struct_size = (uint32_t)sizeof owner;
    owner.kind = AYNI_PRINCIPAL_OWNER;
    owner.principal_id = 7u;
    owner.binding_revision = 1u;
    failures += expect(ayni_register_principal(kernel, &owner) == AYNI_OK, "register owner");
    agent.struct_size = (uint32_t)sizeof agent;
    agent.agent_id = 100u;
    failures += expect(ayni_register_agent(kernel, &agent) == AYNI_OK, "register agent");
    goal_type.struct_size = (uint32_t)sizeof goal_type;
    goal_type.version = 1u;
    goal_type.goal_type_id.data = "reminder";
    goal_type.goal_type_id.size = 8u;
    goal_type.validate = accept_parameters;
    failures +=
        expect(ayni_register_goal_type(kernel, &goal_type) == AYNI_OK, "register goal type");

    session.struct_size = (uint32_t)sizeof session;
    session.channel = AYNI_CHANNEL_MANAGEMENT;
    session.principal_id = owner.principal_id;
    failures += expect(ayni_open_session(kernel, &session, &session_id) == AYNI_NOT_READY,
                       "open session in CONFIGURING");
    failures += expect(ayni_close_session(kernel, 1u) == AYNI_INVALID_STATE,
                       "close session in CONFIGURING");

    create.struct_size = (uint32_t)sizeof create;
    create.goal_type_version = 1u;
    create.goal_type_id = goal_type.goal_type_id;
    create.owner_instance_id = 1u;
    intent.struct_size = (uint32_t)sizeof intent;
    intent.kind = AYNI_INTENT_GOAL_CREATE;
    intent.body = &create;
    result.struct_size = (uint32_t)sizeof result;
    error.struct_size = (uint32_t)sizeof error;
    failures += expect(ayni_submit_intent(kernel, &intent, &result, &error) == AYNI_NOT_READY &&
                           error.status == AYNI_NOT_READY && error.reason == AYNI_REASON_NONE,
                       "intent in CONFIGURING");

    agent_info.struct_size = (uint32_t)sizeof agent_info;
    goal_info.struct_size = (uint32_t)sizeof goal_info;
    failures += expect(ayni_get_agent(kernel, agent.agent_id, &agent_info) == AYNI_INVALID_STATE,
                       "agent query in CONFIGURING");
    failures += expect(ayni_get_goal(kernel, 1u, &goal_info) == AYNI_INVALID_STATE,
                       "goal query in CONFIGURING");
    return failures;
}

int main(void) {
    ayni_config_v1 config = {0};
    ayni_host_v1 host = {0};
    ayni_kernel_t *kernel = NULL;
    ayni_lifecycle_t state = 0;
    ayni_kernel_info_v1 info = {0};
    ayni_step_report_v1 report = {0};
    size_t size = 0;
    size_t align = 0;
    int failures = 0;

    config.struct_size = (uint32_t)sizeof config;
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.ingress_capacity = 2u;
    config.event_body_max_bytes = 8u;
    config.completion_slot_count = 1u;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_MIN;
    config.agent_capacity = 1u;
    config.goal_type_capacity = 1u;
    config.goal_capacity = 2u;
    config.goal_parameter_max_bytes = 8u;
    config.session_capacity = 1u;
    config.goal_depth_max = 1u;
    config.goal_children_max = 1u;
    host.struct_size = (uint32_t)sizeof host;
    host.monotonic_now_us = test_clock;
    host.wake = test_wake;
    host.ingress_lock = test_lock;
    host.ingress_unlock = test_unlock;

    failures += expect(ayni_kernel_memory_requirements(&config, &size, &align) == AYNI_OK,
                       "memory requirements");
    failures += expect(size <= sizeof kernel_memory, "kernel fits the static block");
    failures += expect(align <= sizeof(max_aligned_t) && sizeof(max_aligned_t) % align == 0,
                       "static block alignment suffices");
    if (failures != 0) {
        return 1;
    }

    failures +=
        expect(ayni_create(&config, &host, kernel_memory, sizeof kernel_memory, &kernel) == AYNI_OK,
               "create");
    failures +=
        expect(ayni_get_lifecycle(kernel, &state) == AYNI_OK && state == AYNI_LIFECYCLE_CONFIGURING,
               "lifecycle is CONFIGURING");
    failures += expect(ayni_completion_post(NULL, NULL) == AYNI_INVALID_ARGUMENT,
                       "completion post links and rejects NULL");
    /* No storage ports: the kernel cannot recover, so it stays CONFIGURING. */
    failures += expect(ayni_start(kernel) == AYNI_UNSUPPORTED, "start without storage");
    info.struct_size = (uint32_t)sizeof info;
    failures += expect(ayni_get_kernel_info(kernel, &info) == AYNI_OK &&
                           info.lifecycle == AYNI_LIFECYCLE_CONFIGURING && info.boot_epoch == 0u,
                       "kernel info");
    report.struct_size = (uint32_t)sizeof report;
    failures +=
        expect(ayni_step(kernel, 1u, 1u, &report) == AYNI_INVALID_STATE, "step in CONFIGURING");
    failures += check_work_model(kernel);
    failures +=
        expect(ayni_begin_shutdown(kernel) == AYNI_INVALID_STATE, "shutdown in CONFIGURING");
    failures += expect(ayni_destroy(kernel) == AYNI_OK, "destroy");
    return failures == 0 ? 0 : 1;
}
