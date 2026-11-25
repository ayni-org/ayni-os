/* Uses the implemented ABI from strict C99 with caller-provided memory. */

#include <ayni/arguments.h>
#include <ayni/audit.h>
#include <ayni/authority.h>
#include <ayni/ayni.h>
#include <ayni/host_authority.h>
#include <ayni/memory.h>
#include <ayni/observations.h>
#include <ayni/reflex.h>
#include <ayni/resources.h>
#include <ayni/storage_format.h>
#include <ayni/values.h>

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

/* ABI 0.53 reserves owner-thread codec scratch; the query below proves this
 * caller-owned test block suffices, without prescribing a production arena. */
enum {
    kernel_memory_bytes = 1024u * 1024u,
    kernel_memory_units = (kernel_memory_bytes + sizeof(max_aligned_t) - 1u) / sizeof(max_aligned_t)
};
static max_aligned_t kernel_memory[kernel_memory_units];

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

/* A decide function in C, completing every task it decides. It never runs:
 * this kernel never reaches RUNNING. */
static ayni_status_t complete_task(void *ctx, const ayni_task_snapshot_v1 *snapshot,
                                   ayni_policy_output_v1 *output) {
    (void)ctx;
    output->intents[0].struct_size = (uint32_t)sizeof output->intents[0];
    output->intents[0].kind = AYNI_POLICY_REPORT_COMPLETION;
    output->intents[0].expected_revision = snapshot->revision;
    output->intent_count = 1u;
    return AYNI_OK;
}

/* Registration works in CONFIGURING; sessions, intents and queries wait for
 * RUNNING, which this storage-less kernel never reaches. */
static int check_work_model(ayni_kernel_t *kernel) {
    ayni_principal_desc_v1 owner = {0};
    ayni_agent_desc_v1 agent = {0};
    ayni_goal_type_v1 goal_type = {0};
    ayni_goal_authority_info_v1 authority = {0};
    ayni_goal_requirement_info_v1 requirement = {0};
    ayni_goal_requirement_field_info_v1 requirement_field = {0};
    ayni_strategy_v1 strategy = {0};
    ayni_session_desc_v1 session = {0};
    ayni_goal_create_v1 create = {0};
    ayni_intent_v1 intent = {0};
    ayni_intent_result_v1 result = {0};
    ayni_intent_receipt_v1 receipt = {0};
    ayni_error_v1 error = {0};
    ayni_agent_info_v1 agent_info = {0};
    ayni_goal_info_v1 goal_info = {0};
    ayni_task_info_v1 task_info = {0};
    ayni_grant_info_v1 grant_info = {0};
    ayni_grant_field_info_v1 grant_field = {0};
    ayni_task_authority_info_v1 task_authority = {0};
    ayni_task_authority_reference_v1 task_reference = {0};
    uint32_t parameter_size = 0;
    ayni_goal_approve_v1 approval = {0};
    ayni_goal_reauthorize_v1 reauthorization = {0};
    uint64_t session_id = 0;
    int failures = 0;
    reauthorization.struct_size = sizeof(reauthorization);
    reauthorization.goal_id = 1;
    reauthorization.expected_authority_commit_seq = 1;
    reauthorization.original = create;
    (void)reauthorization;

    approval.struct_size = sizeof approval;
    approval.lifetime = AYNI_GRANT_PERMANENT;
    failures += expect(approval.duration_us == 0u, "permanent approval duration");
    failures += expect(ayni_get_goal_parameters(kernel, 1u, NULL, 0u, &parameter_size) ==
                           AYNI_INVALID_STATE,
                       "parameters before start");
    owner.struct_size = (uint32_t)sizeof owner;
    owner.kind = AYNI_PRINCIPAL_OWNER;
    owner.principal_id = 7u;
    owner.binding_revision = 1u;
    failures += expect(ayni_register_principal(kernel, &owner) == AYNI_OK, "register owner");
    agent.struct_size = (uint32_t)sizeof agent;
    agent.agent_id = 100u;
    failures += expect(ayni_register_agent(kernel, &agent) == AYNI_OK, "register agent");
    authority.struct_size = sizeof authority;
    requirement.struct_size = sizeof requirement;
    requirement_field.struct_size = sizeof requirement_field;
    failures += expect(ayni_get_goal_authority(kernel, 1u, &authority) == AYNI_INVALID_STATE,
                       "authority before start");
    failures +=
        expect(ayni_get_goal_requirement(kernel, 1u, 0u, &requirement) == AYNI_INVALID_STATE,
               "requirement before start");
    failures += expect(ayni_get_goal_requirement_field(kernel, 1u, 0u, 0u, &requirement_field) ==
                           AYNI_INVALID_STATE,
                       "requirement field before start");
    task_authority.struct_size = sizeof task_authority;
    task_reference.struct_size = sizeof task_reference;
    failures += expect(ayni_get_task_authority(kernel, 1u, &task_authority) == AYNI_INVALID_STATE,
                       "Task authority before start");
    failures += expect(ayni_get_task_authority_reference(kernel, 1u, 0u, &task_reference) ==
                           AYNI_INVALID_STATE,
                       "Task authority reference before start");
    goal_type.struct_size = (uint32_t)sizeof goal_type;
    goal_type.version = 1u;
    goal_type.goal_type_id.data = "reminder";
    goal_type.goal_type_id.size = 8u;
    goal_type.validate = accept_parameters;
    goal_type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE) |
                              AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE) |
                              AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL);
    goal_type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    goal_type.strategy_id.data = "complete";
    goal_type.strategy_id.size = 8u;
    failures +=
        expect(ayni_register_goal_type(kernel, &goal_type) == AYNI_OK, "register goal type");
    strategy.struct_size = (uint32_t)sizeof strategy;
    strategy.version = 1u;
    strategy.strategy_id = goal_type.strategy_id;
    strategy.decide = complete_task;
    strategy.max_intents_per_call = 1u;
    strategy.intent_size = (uint32_t)sizeof(ayni_policy_intent_v1);
    failures += expect(ayni_register_strategy(kernel, &strategy) == AYNI_OK, "register strategy");

    session.struct_size = (uint32_t)sizeof session;
    session.channel = AYNI_CHANNEL_MANAGEMENT;
    session.principal_id = owner.principal_id;
    session.binding_revision = owner.binding_revision;
    failures += expect(ayni_open_session(kernel, &session, &session_id) == AYNI_NOT_READY,
                       "open session in CONFIGURING");
    failures += expect(ayni_close_session(kernel, 1u) == AYNI_INVALID_STATE,
                       "close session in CONFIGURING");

    create.struct_size = (uint32_t)sizeof create;
    create.goal_type_version = 1u;
    create.goal_type_id = goal_type.goal_type_id;
    create.owner_instance_id = 1u;
    create.trigger = AYNI_TRIGGER_INTERVAL;
    create.trigger_delay_us = 0u;
    create.trigger_period_us = AYNI_TRIGGER_PERIOD_MIN_US;
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
    task_info.struct_size = (uint32_t)sizeof task_info;
    failures += expect(ayni_get_agent(kernel, agent.agent_id, &agent_info) == AYNI_INVALID_STATE,
                       "agent query in CONFIGURING");
    failures += expect(ayni_get_goal(kernel, 1u, &goal_info) == AYNI_INVALID_STATE,
                       "goal query in CONFIGURING");
    failures += expect(ayni_get_task(kernel, 1u, &task_info) == AYNI_INVALID_STATE,
                       "task query in CONFIGURING");
    grant_info.struct_size = (uint32_t)sizeof grant_info;
    failures += expect(grant_info.bound_goal_id == 0u, "generic Grant has no Goal binding");
    grant_field.struct_size = (uint32_t)sizeof grant_field;
    failures += expect(ayni_get_grant(kernel, 1u, &grant_info) == AYNI_INVALID_STATE,
                       "grant query links from C99 in CONFIGURING");
    failures += expect(ayni_get_grant_field(kernel, 1u, 0u, &grant_field) == AYNI_INVALID_STATE,
                       "grant field query links from C99 in CONFIGURING");
    receipt.struct_size = (uint32_t)sizeof receipt;
    failures += expect(ayni_get_intent_receipt(kernel, owner.principal_id, 1u, &receipt) ==
                           AYNI_INVALID_STATE,
                       "intent receipt query links from C99 in CONFIGURING");
    return failures;
}

int main(void) {
    ayni_config_v1 config = {0};
    ayni_host_v1 host = {0};
    ayni_kernel_t *kernel = NULL;
    ayni_lifecycle_t state = 0;
    ayni_kernel_info_v1 info = {0};
    ayni_step_report_v1 report = {0};
    ayni_event_v1 event = {0};
    size_t size = 0;
    size_t align = 0;
    int failures = 0;

    /* ADR-0040: prefix inspection links through the strict C99 public API. */
    {
        const uint8_t prefix[AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES] = {'A', 'Y', 'J', 'B'};
        ayni_storage_batch_identity_v1 identity = {1u, 1u, 1u};
        failures +=
            expect(ayni_storage_read_batch_identity(prefix, sizeof prefix, &identity) == 1 &&
                       identity.writer_epoch == 0 && identity.commit_seq == 0 &&
                       identity.transition_id == 0,
                   "storage identity prefix links from C99 without semantic validation");
    }

    config.struct_size = (uint32_t)sizeof config;
    config.abi_major = AYNI_ABI_MAJOR;
    config.abi_minor = AYNI_ABI_MINOR;
    config.ingress_capacity = 2u;
    config.event_body_max_bytes = 8u;
    config.completion_slot_count = 1u;
    config.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
    config.agent_capacity = 1u;
    config.goal_type_capacity = 1u;
    config.goal_capacity = 2u;
    config.goal_parameter_max_bytes = 8u;
    config.session_capacity = 1u;
    config.goal_depth_max = 1u;
    config.goal_children_max = 1u;
    config.task_capacity = 1u;
    config.strategy_capacity = 1u;
    config.policy_intent_capacity = 1u;
    config.task_step_capacity = 1u;
    config.task_checkpoint_max_bytes = 0u;
    config.command_capacity = 1u;
    config.capability_capacity = 1u;
    config.resource_capacity = 1u;
    config.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN;
    config.grant_capacity = 1u;
    config.grant_delegation_depth_max = AYNI_GRANT_DELEGATION_DEPTH_MAX_DEFAULT;
    config.goal_requirement_capacity = AYNI_GOAL_REQUIREMENT_CAPACITY_DEFAULT;
    config.storage_write_interval_us = AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT;
    config.storage_log_max_bytes = AYNI_STORAGE_LOG_MAX_BYTES_DEFAULT;
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
                           info.lifecycle == AYNI_LIFECYCLE_CONFIGURING && info.boot_epoch == 0u &&
                           info.events_unmatched == 0u && info.events_discarded == 0u,
                       "kernel info");
    event.struct_size = (uint32_t)sizeof event;
    event.kind = AYNI_EVENT_LOCAL_INPUT;
    event.source.data = "button.record";
    event.source.size = 13u;
    failures +=
        expect(ayni_post_event(kernel, &event) == AYNI_INVALID_STATE, "event in CONFIGURING");
    report.struct_size = (uint32_t)sizeof report;
    failures +=
        expect(ayni_step(kernel, 1u, 1u, &report) == AYNI_INVALID_STATE, "step in CONFIGURING");
    {
        ayni_capability_desc_v1 capability = {0};
        capability.struct_size = (uint32_t)sizeof capability;
        capability.version = 1u;
        capability.capability_id.data = "notify.present";
        capability.capability_id.size = 14u;
        failures += expect(ayni_register_capability(kernel, &capability) == AYNI_OK,
                           "capability schema registration links from C99");
    }
    failures += check_work_model(kernel);
    /* C-RES-013: trusted resource registration, health and copied diagnostics
     * are linkable from C99; no call or grant is implied. */
    {
        ayni_resource_desc_v1 resource = {0};
        ayni_resource_info_v1 resource_info = {0};
        resource.struct_size = sizeof resource;
        resource.kind = AYNI_RESOURCE_SHARED;
        resource.resource_id.data = "model";
        resource.resource_id.size = 5u;
        resource.capacity = 1u;
        resource_info.struct_size = sizeof resource_info;
        failures +=
            expect(ayni_register_resource(kernel, &resource) == AYNI_OK, "register resource");
        failures += expect(
            ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE) == AYNI_OK,
            "set resource health");
        failures +=
            expect(ayni_get_resource(kernel, resource.resource_id, &resource_info) == AYNI_OK &&
                       resource_info.state == AYNI_RESOURCE_FREE,
                   "query resource");
    }
    /* ADR-0112 (ABI 0.59): lock-free observation slot and reflex rule symbols link from C99. */
    {
        ayni_observation_slot_info_v1 slot_info = {0};
        ayni_reflex_rule_v1 rule = {0};
        ayni_reflex_info_v1 reflex_info = {0};
        ayni_reflex_event_v1 reflex_event = {0};
        const ayni_string_v1 rule_id = {"reflex.smoke", 12u};
        slot_info.struct_size = (uint32_t)sizeof slot_info;
        rule.struct_size = (uint32_t)sizeof rule;
        reflex_info.struct_size = (uint32_t)sizeof reflex_info;
        reflex_event.struct_size = (uint32_t)sizeof reflex_event;
        failures += expect(ayni_get_observation_slots(kernel, &slot_info) == AYNI_OK &&
                               slot_info.source_count == 0u,
                           "observation slot snapshot links from C99");
        failures += expect(ayni_register_reflex_rule(kernel, &rule) == AYNI_INVALID_ARGUMENT,
                           "reflex registration links and rejects malformed rule");
        failures += expect(ayni_set_reflex_enabled(kernel, rule_id, 1u) == AYNI_NOT_READY,
                           "reflex runtime toggle links from C99 in CONFIGURING");
        failures += expect(ayni_get_reflex_info(kernel, rule_id, &reflex_info) == AYNI_NOT_FOUND,
                           "reflex info links from C99");
        failures += expect(ayni_take_reflex_event(kernel, &reflex_event) == AYNI_NOT_FOUND,
                           "reflex event drain links from C99");
    }
    /* ADR-0113 (ABI 0.60): asynchronous RAM audit ring symbols link from C99. */
    {
        ayni_audit_info_v1 audit_info = {0};
        ayni_audit_record_v1 record = {0};
        uint32_t count = 1u;
        audit_info.struct_size = (uint32_t)sizeof audit_info;
        failures += expect(ayni_get_audit_info(kernel, &audit_info) == AYNI_OK &&
                               audit_info.capacity == 0u && audit_info.pending == 0u,
                           "audit info links from C99");
        failures +=
            expect(ayni_take_audit_records(kernel, &record, 1u, &count) == AYNI_OK && count == 0u,
                   "audit take links from C99");
    }
    failures +=
        expect(ayni_begin_shutdown(kernel) == AYNI_INVALID_STATE, "shutdown in CONFIGURING");
    failures += expect(ayni_destroy(kernel) == AYNI_OK, "destroy");
    {
        /* C-USAGE-006: pure UTF-8 also links from C99 without a live Kernel. */
        const ayni_string_v1 valid = {"\0\xe4\xb8\x96", 4u};
        const ayni_string_v1 invalid = {"\xed\xa0\x80", 3u};
        failures += expect(ayni_validate_utf8(valid) == AYNI_OK &&
                               ayni_validate_utf8(invalid) == AYNI_INVALID_ARGUMENT,
                           "pure Unicode scalar validation");
    }
    {
        /* ABI 0.45: pure civil values also work without a live Kernel. */
        const ayni_string_v1 zone = {"CST-8", 5u};
        ayni_calendar_civil_v1 civil = {0};
        int64_t local = 0;
        civil.struct_size = sizeof civil;
        failures +=
            expect(ayni_calendar_local_time(zone, 0, &local) == AYNI_OK &&
                       ayni_calendar_split_time(local, &civil) == AYNI_OK && civil.year == 1970 &&
                       civil.month == 1 && civil.day == 1 && civil.hour == 8,
                   "pure calendar conversion");
    }
    return failures == 0 ? 0 : 1;
}
