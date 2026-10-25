// ADR-0050 / C-PERMIT-002/005 / C-STOP-006: public normal execution and declared boundaries.
// No power loss, injected transport failure or restart campaign.
#include "kernel_test_probe.h"
#include "work_node.h"
#include <ayni/providers/mock_execution.h>
#include <ayni/scenes.h>
namespace {
using namespace ayni::test;
using ayni::providers::mock_execution::Executor;
using ayni::providers::mock_execution::StopConfirmation;
ayni_catalog_name_v1 name(const char *value) {
    ayni_catalog_name_v1 out{};
    out.size = static_cast<uint32_t>(std::strlen(value));
    std::memcpy(out.data, value, out.size);
    return out;
}
class ManagedExecution : public ::testing::Test {
  protected:
    void SetUp() override {
        auto &config = work.node().config();
        config.call_capacity = 4;
        config.call_input_max_bytes = 256;
        config.call_result_max_bytes = 64;
        config.call_capability_capacity = 1;
        config.provider_capacity = 1;
        config.binding_capacity = 1;
        config.resource_capacity = 1;
        config.resource_request_capacity = 4;
        config.goal_type_capacity = 2;
        config.scene_capacity = 1;
        config.observation_source_capacity = 1;
        config.executor_capacity = 1;
        config.execution_binding_capacity = 1;
        config.storage_buffer_bytes = 0;
        work.node().on_create([this](auto *kernel) { register_profile(kernel); });
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            task_id = snapshot.task_id;
            ayni_task_call_result_v1 result{};
            result.struct_size = sizeof(result);
            const auto found = snapshot.call_result(snapshot.call_context, text("turn"), &result);
            if (found == AYNI_OK) {
                return report(result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                    : AYNI_POLICY_REPORT_FAILURE,
                              snapshot, output);
            }
            if (found != AYNI_NOT_FOUND) {
                return found;
            }
            ayni_scope_field_v1 field{};
            field.struct_size = sizeof(field);
            field.field_id = 1;
            field.allowed.struct_size = sizeof(field.allowed);
            field.allowed.kind = AYNI_AUTHORITY_I64;
            field.allowed.unit = text("mrad");
            field.allowed.values.integer = {5, 5};
            const ayni_call_request_v1 request{
                sizeof(request),      AYNI_POLICY_REQUEST_CALL,           1, 0, text("turn"),
                text("turn.binding"), {sizeof(ayni_scope_v1), 1, &field}, {}};
            return output.request_call(output.call_context, &request);
        };
    }
    void start(ayni_execution_form_t form = AYNI_FORM_PROCEDURE, uint32_t ticks = 8,
               StopConfirmation confirmation = StopConfirmation::automatic,
               uint32_t timeout_ms = 30000) {
        ayni::providers::mock_execution::Config config;
        config.clock_context = this;
        config.now_us = [](void *context) { return static_cast<ManagedExecution *>(context)->now; };
        config.ticks_to_complete = ticks;
        config.call_timeout_ms = timeout_ms;
        config.stop_confirmation = confirmation;
        config.limits.count = 1;
        config.limits.fields[0] = {name("yaw"), name("mrad"), -100, 100};
        ASSERT_EQ(executor.configure(config), AYNI_OK);
        ASSERT_EQ(executor.describe_provider(provider), AYNI_OK);
        execution_form = form;
        work.run();
        for (auto agent : {agent_a, agent_b}) {
            const ayni_grant_create_v1 grant{sizeof(grant),
                                             1,
                                             text("mock.turn"),
                                             work.instance(agent),
                                             0,
                                             AYNI_GRANT_PERMANENT,
                                             0,
                                             0,
                                             {sizeof(ayni_scope_v1), 0, nullptr}};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
            intent.command_id = ++command;
            const auto accepted = work.submit(intent);
            ASSERT_EQ(accepted.status, AYNI_OK);
            grants.push_back(accepted.result.object.id);
        }
        change_scene(true);
        pump(20);
        observe(1);
    }
    void register_profile(ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        for (auto agent : {agent_a, agent_b}) {
            const auto descriptor = agent_desc(agent);
            ASSERT_EQ(ayni_register_agent(kernel, &descriptor), AYNI_OK);
        }
        const auto strategy =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
        auto type = goal_type_desc("turn", 1);
        type.export_requirements = [](void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
            const ayni_goal_requirement_v1 required{
                sizeof(required), 1, text("mock.turn"), {sizeof(ayni_scope_v1), 0, nullptr}};
            return sink->emit(sink->ctx, &required);
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
        type.goal_type_id = text("foreground");
        type.work_profile = {AYNI_WORK_FOREGROUND, 0, 0};
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
        ayni_authority_field_desc_v1 field{};
        field.struct_size = sizeof(field);
        field.field_id = 1;
        field.usage = AYNI_AUTHORITY_PARAMETER;
        field.domain.struct_size = sizeof(field.domain);
        field.domain.kind = AYNI_AUTHORITY_I64;
        field.domain.unit = text("mrad");
        field.domain.values.integer = {-100, 100};
        const ayni_capability_desc_v1 capability{
            sizeof(capability), 1, text("mock.turn"), 1, 0, &field};
        ASSERT_EQ(ayni_register_capability(kernel, &capability), AYNI_OK);
        const ayni_resource_desc_v1 resource{sizeof(resource),
                                             AYNI_RESOURCE_EXCLUSIVE,
                                             text("mock.actuator"),
                                             1,
                                             AYNI_RESOURCE_ACTUATOR,
                                             0,
                                             0,
                                             nullptr,
                                             {}};
        ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        ASSERT_EQ(ayni_register_call_capability(kernel, provider.capabilities), AYNI_OK);
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("turn.binding"),
                                      capability.capability_id,
                                      1,
                                      1,
                                      provider.provider_id,
                                      provider.endpoint_id,
                                      {}};
        ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        const ayni_observation_source_v1 source{
            sizeof(source), 1,      text("ready"), AYNI_OBSERVATION_BOOL, AYNI_OBSERVATION_MEASURED,
            text("bool"),   5000000};
        ASSERT_EQ(ayni_register_observation_source(kernel, &source), AYNI_OK);
        const ayni_scene_rule_v1 rule{
            capability.capability_id, 1, 0, {sizeof(ayni_scope_v1), 0, nullptr}};
        const ayni_observation_condition_v1 condition{text("ready"), 1, 0, 1, 1, 5000000};
        const ayni_scene_desc_v1 scene{sizeof(scene), 1, text("look"), 1, 1, &rule, &condition};
        ASSERT_EQ(ayni_register_scene(kernel, &scene), AYNI_OK);
        ayni_executor_v1 ports{};
        ASSERT_EQ(executor.describe_executor(ports), AYNI_OK);
        ASSERT_EQ(ayni_register_executor(kernel, &ports), AYNI_OK);
        const ayni_execution_field_v1 mapping{text("yaw"), text("mrad"), 1, 1};
        const ayni_execution_binding_v1 execution{sizeof(execution),
                                                  1,
                                                  binding.binding_id,
                                                  ports.executor_id,
                                                  1,
                                                  1,
                                                  text("look"),
                                                  execution_form,
                                                  200000,
                                                  100000,
                                                  1,
                                                  &mapping};
        ASSERT_EQ(ayni_register_execution_binding(kernel, &execution), AYNI_OK);
    }
    void change_scene(bool enable) {
        ayni_scene_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_scene(work.kernel(), text("look"), &info), AYNI_OK);
        const ayni_scene_set_enabled_v1 body{sizeof(body), enable ? 1U : 0U, text("look"),
                                             enable ? 1U : 0U, 0};
        auto intent = work.intent(AYNI_INTENT_SCENE_SET_ENABLED, &body, info.revision);
        intent.command_id = ++command;
        ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    }
    void observe(uint64_t sequence, int64_t value = 1) {
        const ayni_observation_v1 sample{
            sizeof(sample),        1, text("ready"), 1, sequence, now, 5000000,
            AYNI_OBSERVATION_GOOD, 0, value};
        ASSERT_EQ(ayni_publish_observation(work.kernel(), &sample), AYNI_OK);
    }
    void pump(unsigned rounds) {
        for (unsigned at = 0; at < rounds; ++at) {
            now += 10000;
            work.node().sim().advance_us(10000);
            work.node().sim().complete_storage();
            ASSERT_EQ(executor.advance(), AYNI_OK);
            work.node().step();
            ASSERT_NE(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        }
    }
    uint64_t create(uint64_t agent = agent_a) {
        const auto accepted = work.create(
            WorkNode::create_body(work.instance(agent), 0, "turn", 1, {}, AYNI_TRIGGER_ONCE));
        EXPECT_EQ(accepted.status, AYNI_OK);
        return accepted.result.object.id;
    }
    void wait_submission() {
        for (unsigned round = 0; round < 100 && executor.submissions() == 0; ++round) {
            pump(1);
        }
        ASSERT_EQ(executor.submissions(), 1U);
    }
    ayni_execution_info_v1 execution() {
        ayni_execution_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_execution(work.kernel(), executor.logical_call(), &out), AYNI_OK);
        return out;
    }
    ayni_resource_info_v1 resource() {
        ayni_resource_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_resource(work.kernel(), text("mock.actuator"), &out), AYNI_OK);
        return out;
    }
    ayni_control_info_v1 gate() {
        ayni_control_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(executor.info(out), AYNI_OK);
        return out;
    }
    void TearDown() override {
        if (work.node().kernel() == nullptr) {
            return;
        }
        if (work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED) {
            EXPECT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
            for (unsigned round = 0;
                 round < 150 && work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED; ++round) {
                if (gate().phase == AYNI_CONTROL_STOPPING) {
                    EXPECT_EQ(executor.confirm_stopped(), AYNI_OK);
                }
                pump(1);
            }
        }
        EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
        ASSERT_EQ(work.node().destroy(), AYNI_OK);
        EXPECT_EQ(executor.close(), AYNI_OK);
    }
    WorkNode work;
    Executor executor;
    ayni_provider_v1 provider{};
    ayni_execution_form_t execution_form = AYNI_FORM_PROCEDURE;
    uint64_t now = 0;
    uint64_t command = 0;
    uint64_t task_id = 0;
    std::vector<uint64_t> grants;
};
TEST_F(ManagedExecution, procedure_finishes_only_with_independent_stop_and_releases_actuator) {
    start();
    const auto goal = create();
    wait_submission();
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_DISPATCHING);
    EXPECT_LE(execution().requires_seq, work.node().info().durable_seq);
    pump(40);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_EQ(execution().execution_ended, 1U);
    EXPECT_EQ(execution().target, AYNI_TARGET_SATISFIED);
    EXPECT_EQ(executor.samples(), 8U);
    EXPECT_EQ(executor.renewals(), 0U);
    EXPECT_EQ(resource().quarantine_id, 0U);
    EXPECT_EQ(resource().used, 0U);
}
TEST_F(ManagedExecution,
       session_renews_with_fresh_nonce_and_normal_close_never_resumes_old_motion) {
    start(AYNI_FORM_SESSION, 60);
    const auto goal = create();
    wait_submission();
    pump(35);
    EXPECT_EQ(gate().phase, AYNI_CONTROL_ACTIVE);
    EXPECT_GT(executor.renewals(), 1U);
    const auto produced = executor.samples();
    change_scene(false);
    work.node().step();
    EXPECT_EQ(gate().phase, AYNI_CONTROL_STOPPING);
    pump(20);
    EXPECT_EQ(executor.samples(), produced);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_NE(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
    change_scene(true);
    pump(10);
    EXPECT_EQ(executor.submissions(), 1U);
}
TEST_F(ManagedExecution, unconfirmed_stop_retains_terminal_and_quarantine_blocks_other_agent) {
    start(AYNI_FORM_PROCEDURE, 2, StopConfirmation::manual);
    create();
    wait_submission();
    const auto original = executor.logical_call();
    create(agent_b);
    pump(18);
    EXPECT_EQ(executor.pending(), 0U);
    EXPECT_EQ(executor.submissions(), 1U);
    EXPECT_EQ(gate().phase, AYNI_CONTROL_STOPPING);
    EXPECT_EQ(resource().quarantine_id, original);
    EXPECT_EQ(execution().execution_ended, 0U);
    EXPECT_EQ(executor.confirm_stopped(), AYNI_OK);
    pump(20);
    EXPECT_EQ(executor.submissions(), 2U);
}
TEST_F(ManagedExecution, procedure_expires_normally_without_renewal_and_never_revives) {
    start(AYNI_FORM_PROCEDURE, 60);
    create();
    wait_submission();
    pump(45);
    EXPECT_EQ(gate().stop_cause, AYNI_CONTROL_STOP_EXPIRED);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_EQ(execution().target, AYNI_TARGET_UNVERIFIED);
    EXPECT_EQ(executor.renewals(), 0U);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
TEST_F(ManagedExecution, observation_replacement_stops_original_session) {
    start(AYNI_FORM_SESSION, 60);
    create();
    wait_submission();
    pump(5);
    observe(2);
    work.node().step();
    EXPECT_EQ(gate().phase, AYNI_CONTROL_STOPPING);
    pump(20);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
}
TEST_F(ManagedExecution, cancelled_task_is_not_reopened_by_late_completion) {
    start(AYNI_FORM_SESSION, 60);
    create();
    wait_submission();
    const auto original_task = task_id;
    ASSERT_EQ(work.cancel_task(original_task).status, AYNI_OK);
    pump(30);
    EXPECT_EQ(work.task(original_task).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// ADR-0051 decision 11, white-box: the task table reads whether the runtime
// still holds a call's lease or slot, and releasing them after the stop takes
// no transition. With room for one task, the second agent's goal waits for
// the cancelled task. One-item steps let an append's completion take a whole
// step, so the lease goes in a later step that changes neither the commit
// sequence nor the durable prefix; the kept due-work search still answers as
// a new search would, and the goal derives.
TEST_F(ManagedExecution, released_lease_lets_the_waiting_goal_derive) {
    work.node().config().task_capacity = 1;
    start(AYNI_FORM_SESSION, 60);
    create();
    wait_submission();
    const auto original_task = task_id;
    const auto waiting = create(agent_b);
    ASSERT_EQ(work.cancel_task(original_task).status, AYNI_OK);
    for (unsigned round = 0; round < 60; ++round) {
        now += 10000;
        work.node().sim().advance_us(10000);
        work.node().sim().complete_storage();
        ASSERT_EQ(executor.advance(), AYNI_OK);
        work.node().step(1);
        const auto [kept, fresh] =
            ayni::kernel::kept_and_new_due_search(work.kernel(), work.node().sim().now_us());
        ASSERT_TRUE(kept == fresh) << round;
    }
    EXPECT_EQ(resource().quarantine_id, 0U);
    EXPECT_EQ(work.goal(waiting).occurrences, 1U);
}
TEST_F(ManagedExecution, closed_scene_rejects_admission_without_any_executor_output) {
    start();
    change_scene(false);
    create();
    pump(30);
    EXPECT_EQ(executor.submissions(), 0U);
    EXPECT_EQ(executor.samples(), 0U);
    EXPECT_EQ(resource().used, 0U);
}
TEST_F(ManagedExecution, provider_without_permit_refuses_before_retaining_borrow) {
    start();
    ayni_provider_request_v1 request{};
    request.struct_size = sizeof(request);
    EXPECT_EQ(provider.submit(provider.ctx, &request, nullptr), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    EXPECT_EQ(executor.pending(), 0U);
    EXPECT_EQ(executor.samples(), 0U);
}
// C-PERMIT-005: the existing revocation gate stops renewal before journal acknowledgement.
TEST_F(ManagedExecution, grant_revocation_stops_without_waiting_for_durable_cleanup) {
    start(AYNI_FORM_SESSION, 60);
    create();
    wait_submission();
    pump(4);
    const auto renewals = executor.renewals();
    const ayni_grant_revoke_v1 body{sizeof(body), 0, grants[0]};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &body, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    work.node().step();
    EXPECT_EQ(gate().phase, AYNI_CONTROL_STOPPING);
    pump(25);
    EXPECT_EQ(executor.renewals(), renewals);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// C-PERMIT-002: no permit reaches the local gate while an ordinary barrier is pending.
TEST_F(ManagedExecution, dispatch_waits_for_storage_and_stopped_fact_precedes_resource_reuse) {
    start();
    create();
    for (unsigned at = 0; at < 20; ++at) {
        work.node().step();
    }
    EXPECT_EQ(executor.submissions(), 0U);
    EXPECT_EQ(gate().phase, AYNI_CONTROL_IDLE);
    wait_submission();
    EXPECT_LE(execution().requires_seq, work.node().info().durable_seq);
    pump(40);
    EXPECT_LE(execution().requires_seq, work.node().info().durable_seq);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// ADR-0050: priority can reorder accepted operation IDs; permit anti-replay remains strict.
TEST_F(ManagedExecution, priority_reordering_keeps_distinct_monotonic_dispatch_permits) {
    start();
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("mock.actuator"), AYNI_RESOURCE_OFFLINE),
              AYNI_OK);
    const auto ordinary = create();
    pump(12);
    const auto accepted = work.create(
        WorkNode::create_body(work.instance(agent_b), 0, "foreground", 1, {}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(accepted.status, AYNI_OK);
    pump(12);
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("mock.actuator"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    wait_submission();
    const auto first_operation = execution().operation_id;
    const auto first_permit = execution().permit_id;
    pump(50);
    EXPECT_EQ(executor.submissions(), 2U);
    EXPECT_LT(execution().operation_id, first_operation);
    EXPECT_GT(execution().permit_id, first_permit);
    EXPECT_EQ(work.goal(ordinary).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(work.goal(accepted.result.object.id).lifecycle, AYNI_GOAL_SATISFIED);
}
// C-CALL-008: result application rechecks the exact observation, independently of stop proof.
TEST_F(ManagedExecution, replaced_dependency_cannot_publish_completed_step_after_execution_ended) {
    start(AYNI_FORM_PROCEDURE, 2, StopConfirmation::manual);
    const auto goal = create();
    wait_submission();
    pump(12);
    ASSERT_EQ(executor.pending(), 0U);
    observe(2);
    ASSERT_EQ(executor.confirm_stopped(), AYNI_OK);
    pump(20);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_FAILED);
    ayni_call_info_v1 call{};
    call.struct_size = sizeof(call);
    ASSERT_EQ(ayni_get_call(work.kernel(), executor.logical_call(), &call), AYNI_OK);
    EXPECT_EQ(call.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(call.reason, AYNI_REASON_EXECUTION_OBSERVATION);
}
// ADR-0050: catalog validation fails atomically and never expands configured storage.
TEST(ExecutionCatalog, malformed_registration_does_not_consume_slots_and_capacity_is_explicit) {
    Executor executor;
    ayni::providers::mock_execution::Config config;
    config.now_us = [](void *) -> uint64_t { return 0; };
    config.limits.count = 1;
    config.limits.fields[0] = {name("yaw"), name("mrad"), -10, 10};
    ASSERT_EQ(executor.configure(config), AYNI_OK);
    Node node;
    node.config().executor_capacity = 1;
    node.config().execution_binding_capacity = 1;
    node.create();
    ayni_executor_v1 ports{};
    ASSERT_EQ(executor.describe_executor(ports), AYNI_OK);
    auto invalid = ports;
    invalid.poll_period_us = 0;
    EXPECT_EQ(ayni_register_executor(node.kernel(), &invalid), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_register_executor(node.kernel(), &ports), AYNI_OK);
    ports.executor_id = text("second");
    ports.control_domain_id = 2;
    EXPECT_EQ(ayni_register_executor(node.kernel(), &ports), AYNI_CAPACITY_EXCEEDED);
    const ayni_execution_field_v1 field{text("yaw"), text("mrad"), 1, 1};
    ayni_execution_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("binding"),
                                      text("mock.executor"),
                                      1,
                                      1,
                                      text("scene"),
                                      AYNI_FORM_PROCEDURE,
                                      0,
                                      1000,
                                      1,
                                      &field};
    EXPECT_EQ(ayni_register_execution_binding(node.kernel(), &binding), AYNI_INVALID_ARGUMENT);
    binding.lease_us = 100000;
    EXPECT_EQ(ayni_register_execution_binding(node.kernel(), &binding), AYNI_OK);
    binding.binding_id = text("second");
    EXPECT_EQ(ayni_register_execution_binding(node.kernel(), &binding), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(node.destroy(), AYNI_OK);
    EXPECT_EQ(executor.close(), AYNI_OK);
}
// C-PERMIT-005 / ADR-0050: a renewable local lease cannot extend the original call deadline.
TEST_F(ManagedExecution, call_deadline_stops_session_independently_of_lease_renewals) {
    start(AYNI_FORM_SESSION, 60, StopConfirmation::automatic, 200);
    create();
    wait_submission();
    pump(25);
    EXPECT_EQ(gate().phase, AYNI_CONTROL_STOPPED);
    EXPECT_EQ(gate().stop_cause, AYNI_CONTROL_STOP_REQUESTED);
    EXPECT_LT(executor.samples(), 60U);
    const auto renewed = executor.renewals();
    pump(10);
    EXPECT_EQ(executor.renewals(), renewed);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
}
// ADR-0050: one-item steps must make business progress even at every poll deadline.
TEST_F(ManagedExecution, one_item_steps_share_routine_polling_with_terminal_processing) {
    start(AYNI_FORM_SESSION, 8);
    const auto goal = create();
    wait_submission();
    for (unsigned at = 0; at < 120 && work.goal(goal).lifecycle != AYNI_GOAL_SATISFIED; ++at) {
        now += 20000;
        work.node().sim().advance_us(20000);
        work.node().sim().complete_storage();
        ASSERT_EQ(executor.advance(), AYNI_OK);
        const auto report = work.node().step(1);
        EXPECT_LE(report.items_done, 1U);
    }
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
} // namespace
