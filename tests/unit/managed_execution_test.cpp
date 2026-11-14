#include "workspace_fixture.h"
// ADR-0050 / C-PERMIT-002/005 / C-STOP-006: public normal execution and declared boundaries.
// ADR-0052: executor answers held, lost or late through the Mock's link. No
// power loss, Provider transport failure or restart campaign.
#include "kernel_test_probe.h"
#include "work/call/call_records.h"
#include "work_node.h"
#include <ayni/providers/mock_execution.h>
#include <ayni/scenes.h>
namespace {
using namespace ayni::test;
using ayni::providers::mock_execution::Answers;
using ayni::providers::mock_execution::Executor;
using ayni::providers::mock_execution::PermitStart;
using ayni::providers::mock_execution::StopConfirmation;
constexpr uint64_t tick_us = 10000;   // Clock advance of one pump or step_core round.
constexpr uint32_t lease_us = 200000; // The registered execution binding's lease.
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
            ayni_call_request_v1 request{sizeof(request),
                                         AYNI_POLICY_REQUEST_CALL,
                                         1,
                                         0,
                                         text("turn"),
                                         text("turn.binding"),
                                         {},
                                         {},
                                         {},
                                         AYNI_CALL_MODE_FOREGROUND,
                                         0};
            std::array<ayni_value_node_v1, 5> values{};
            values[0].kind = AYNI_VALUE_OBJECT;
            values[0].subtree_end = 2;
            values[1].kind = AYNI_VALUE_I64;
            values[1].field_id = 1;
            values[1].subtree_end = 2;
            values[1].value.integer = 5;
            request.value = {values.data(), 2};
            if (typed_scene_list) {
                values[0].kind = AYNI_VALUE_OBJECT;
                values[0].subtree_end = 5;
                values[1].kind = AYNI_VALUE_I64;
                values[1].field_id = 1;
                values[1].subtree_end = 2;
                values[1].value.integer = 5;
                values[2].kind = AYNI_VALUE_LIST;
                values[2].field_id = 2;
                values[2].subtree_end = 5;
                for (uint32_t at = 3; at < values.size(); ++at) {
                    values[at].kind = AYNI_VALUE_I64;
                    values[at].subtree_end = at + 1U;
                    values[at].value.integer = at == 3U ? 5 : second_scene_item;
                }
                request.arguments = {};
                request.value = {values.data(), static_cast<uint32_t>(values.size())};
            }
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
        config.answers = answers;
        config.permit_start = permit_start;
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
        std::array<ayni_authority_field_desc_v1, 2> authority_fields{field, field};
        authority_fields[1].field_id = 2;
        const ayni_capability_desc_v1 capability{
            sizeof(capability),         1, text("mock.turn"),
            typed_scene_list ? 2U : 1U, 0, authority_fields.data()};
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
        auto call = *provider.capabilities;
        auto registered_provider = provider;
        std::array<ayni_schema_node_v1, 4> input_schema{};
        input_schema[0].kind = AYNI_VALUE_OBJECT;
        input_schema[0].subtree_end = 4;
        input_schema[1].kind = AYNI_VALUE_I64;
        input_schema[1].field_id = 1;
        input_schema[1].subtree_end = 2;
        input_schema[1].flags = AYNI_SCHEMA_REQUIRED;
        input_schema[1].name = text("yaw");
        input_schema[1].unit = text("mrad");
        input_schema[1].min_i64 = -100;
        input_schema[1].max_i64 = 100;
        input_schema[2].kind = AYNI_VALUE_LIST;
        input_schema[2].field_id = 2;
        input_schema[2].subtree_end = 4;
        input_schema[2].flags = AYNI_SCHEMA_REQUIRED;
        input_schema[2].name = text("checks");
        input_schema[2].min_count = 1;
        input_schema[2].max_count = 2;
        input_schema[3] = input_schema[1];
        input_schema[3].field_id = 0;
        input_schema[3].subtree_end = 4;
        input_schema[3].flags = 0;
        input_schema[3].name = {};
        ayni_schema_node_v1 result_schema{};
        result_schema.kind = AYNI_VALUE_TEXT;
        result_schema.subtree_end = 1;
        result_schema.max_count = 64;
        std::array<ayni_authority_projection_v1, 2> paths{};
        paths[0].field_id = 1;
        paths[0].path_count = 1;
        paths[0].path[0] = 1;
        paths[0].unit = text("mrad");
        paths[1].field_id = 2;
        paths[1].path_count = 2;
        paths[1].path[0] = 2;
        paths[1].path[1] = AYNI_PROJECTION_LIST_ITEM;
        paths[1].unit = text("mrad");
        const ayni_business_contract_v1 business{
            {input_schema.data(), 4}, {&result_schema, 1}, 2, 0, paths.data()};
        if (typed_scene_list) {
            call.contract.input_format = AYNI_CALL_INPUT_VALUE;
            call.contract.input_text_limit = 256;
            call.business = &business;
        }
        registered_provider.capabilities = &call;
        ASSERT_EQ(ayni_register_call_capability(kernel, &call), AYNI_OK);
        ASSERT_EQ(ayni_register_provider(kernel, &registered_provider), AYNI_OK);
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("turn.binding"),
                                      capability.capability_id,
                                      1,
                                      1,
                                      provider.provider_id,
                                      provider.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        const ayni_observation_source_v1 source{
            sizeof(source), 1,      text("ready"), AYNI_OBSERVATION_BOOL, AYNI_OBSERVATION_MEASURED,
            text("bool"),   5000000};
        ASSERT_EQ(ayni_register_observation_source(kernel, &source), AYNI_OK);
        ayni_scope_field_v1 scene_limit{};
        scene_limit.struct_size = sizeof(scene_limit);
        scene_limit.field_id = 2;
        scene_limit.allowed = field.domain;
        scene_limit.allowed.values.integer = {-10, 10};
        const ayni_scene_rule_v1 rule{capability.capability_id,
                                      1,
                                      0,
                                      {sizeof(ayni_scope_v1), typed_scene_list ? 1U : 0U,
                                       typed_scene_list ? &scene_limit : nullptr}};
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
                                                  lease_us,
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
            now += tick_us;
            work.node().sim().advance_us(tick_us);
            work.node().sim().complete_storage();
            ASSERT_EQ(executor.advance(), AYNI_OK);
            work.node().step();
            ASSERT_NE(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        }
    }
    // Held answers over a working link: each round delivers what Core asked
    // in the previous one, well inside the poll period.
    void serve(unsigned rounds) {
        for (unsigned at = 0; at < rounds; ++at) {
            static_cast<void>(executor.answer());
            pump(1);
        }
    }
    // Core alone: its clock moves, the executor's work loop does not run.
    void step_core(unsigned rounds = 1) {
        for (unsigned at = 0; at < rounds; ++at) {
            now += tick_us;
            work.node().sim().advance_us(tick_us);
            work.node().sim().complete_storage();
            work.node().step();
        }
    }
    uint64_t create(uint64_t agent = agent_a, uint32_t pause_group = 0) {
        auto body =
            WorkNode::create_body(work.instance(agent), 0, "turn", 1, {}, AYNI_TRIGGER_ONCE);
        body.pause_group = pause_group;
        const auto accepted = work.create(body);
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
                static_cast<void>(executor.answer());
                pump(1);
            }
        }
        EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
        ASSERT_EQ(work.node().destroy(), AYNI_OK);
        EXPECT_EQ(executor.close(), AYNI_OK);
    }
    bool typed_scene_list = false;
    int64_t second_scene_item = 50;
    WorkNode work;
    Executor executor;
    ayni_provider_v1 provider{};
    ayni_execution_form_t execution_form = AYNI_FORM_PROCEDURE;
    Answers answers = Answers::immediate;
    PermitStart permit_start = PermitStart::at_submit;
    uint64_t now = 0;
    uint64_t command = 0;
    uint64_t task_id = 0;
    std::vector<uint64_t> grants;
};

// White-box, ADR-0050 / C-PERMIT-002: a decoded physical Call must retain
// the original binding/epoch/Scene and the exact possibility of permit dispatch.
TEST_F(ManagedExecution, physical_call_record_rejects_changed_permit_identity_and_phase_proof) {
    start(AYNI_FORM_SESSION, 200);
    create();
    wait_submission();
    const auto &calls = ayni::kernel::KernelTestProbe::work(work.kernel()).calls;
    uint32_t index = 0;
    ASSERT_TRUE(calls.find(executor.logical_call(), index));
    const auto original = calls.record(index);
    ASSERT_TRUE(ayni::work::CallTable::valid_record(::ayni::test::codec_workspace(), original));
    ASSERT_TRUE(original.state.execution.has_value());
    const auto changes = {
        +[](ayni::work::CallRecord &r) { r.state.execution.reset(); },
        +[](ayni::work::CallRecord &r) {
            auto &f = *r.state.execution;
            ++f.epochs.kernel_boot_epoch;
            for (uint32_t at = 0; at < f.dependency_count; ++at) {
                f.dependencies[at].boot_epoch = f.epochs.kernel_boot_epoch;
            }
        },
        +[](ayni::work::CallRecord &r) {
            r.state.execution->scene_seq = r.state.created.transition.commit_seq;
        },
        +[](ayni::work::CallRecord &r) {
            EXPECT_TRUE(r.state.execution->binding.binding.assign(text("other")));
        },
        +[](ayni::work::CallRecord &r) { ++r.state.execution->binding.binding_version; },
        +[](ayni::work::CallRecord &r) { r.state.execution->phase = AYNI_EXECUTION_PREPARED; },
        +[](ayni::work::CallRecord &r) {
            r.state.execution->phase = AYNI_EXECUTION_DISPATCHING;
            r.state.attempt = {};
        },
    };
    for (const auto change : changes) {
        auto candidate = original;
        change(candidate);
        EXPECT_FALSE(
            ayni::work::CallTable::valid_record(::ayni::test::codec_workspace(), candidate));
    }
    // The privacy path has no input projection; it must still validate metadata.
    const ayni::work::CallTable::Limits limits{1, original.domain.size, original.input.size,
                                               original.contract.result_text_limit,
                                               original.contract.business_contract.size};
    std::vector<uint64_t> arena(ayni::work::CallTable::storage_size(limits) / sizeof(uint64_t) + 1);
    ayni::work::CallTable copy(arena.data(), limits);
    ASSERT_EQ(copy.restore_validated(original), AYNI_OK);
    auto stamp = original.state.latest;
    ++stamp.transition.transition_id;
    ++stamp.transition.commit_seq;
    ASSERT_EQ(copy.erase_content(0, stamp), AYNI_OK);
    auto erased = copy.record(0);
    ASSERT_TRUE(ayni::work::CallTable::valid_record(::ayni::test::codec_workspace(), erased));
    erased.state.execution->control_domain = 0;
    EXPECT_FALSE(ayni::work::CallTable::valid_record(::ayni::test::codec_workspace(), erased));
}
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
// ADR-0103/C-STOP-006: white-box erasure closes the gate before ordinary Call work; the
// coordinated Privacy journal/replay flow is covered separately by the existing tests.
TEST_F(ManagedExecution, erased_input_immediately_latches_the_existing_execution_for_stop) {
    start(AYNI_FORM_SESSION, 60);
    create();
    wait_submission();
    pump(5);
    change_scene(true); // An already-enabled Scene retains its generation.
    auto &model = ayni::kernel::KernelTestProbe::work(work.kernel());
    auto *lease =
        ayni::kernel::KernelTestProbe::permits(work.kernel()).find(executor.logical_call());
    ASSERT_NE(lease, nullptr);
    ASSERT_EQ(lease->phase, ayni::resources::PermitLease::Phase::issued);
    uint32_t index = 0;
    ASSERT_TRUE(model.calls.find(executor.logical_call(), index));
    ASSERT_GT(model.transitions.latest().commit_seq,
              model.calls.state(index).latest.transition.commit_seq);
    // White-box gate check at the applied frontier; do not allocate an unjournaled transition.
    ASSERT_EQ(
        model.calls.erase_content(index, {model.ids.boot_epoch(), model.transitions.latest()}),
        AYNI_OK);
    ASSERT_TRUE(ayni::kernel::KernelTestProbe::calls(work.kernel())
                    .refresh_live_gates(work.node().sim().now_us(), work.node().info().durable_seq,
                                        ayni::kernel::RuntimeMode::running));
    EXPECT_EQ(lease->phase, ayni::resources::PermitLease::Phase::stopping);
    const auto produced = executor.samples();
    const auto renewed = executor.renewals();
    ASSERT_EQ(work.cancel_task(task_id).status, AYNI_OK);
    step_core(); // Deliver the latched stop before the executor's next sample.
    pump(20);
    EXPECT_EQ(executor.samples(), produced);
    EXPECT_EQ(executor.renewals(), renewed);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
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
// ADR-0065/C-PERMIT-005: two management controls without step cannot revive old motion.
TEST_F(ManagedExecution, group_pause_then_resume_before_step_still_stops_the_old_session) {
    start(AYNI_FORM_SESSION, 200);
    create(agent_a, 1);
    wait_submission();
    pump(20);
    ASSERT_EQ(gate().phase, AYNI_CONTROL_ACTIVE);
    const auto samples = executor.samples();
    const auto renewals = executor.renewals();
    ayni_pause_group_set_v1 body{sizeof(body), 1, 1, 0};
    auto intent = work.intent(AYNI_INTENT_PAUSE_GROUP_SET, &body, 0);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    body.paused = 0;
    intent.expected_revision = 1;
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    work.node().step();
    pump(20);
    EXPECT_EQ(executor.samples(), samples);
    EXPECT_EQ(executor.renewals(), renewals);
    EXPECT_EQ(executor.submissions(), 1U);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
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
        now += tick_us;
        work.node().sim().advance_us(tick_us);
        work.node().sim().complete_storage();
        ASSERT_EQ(executor.advance(), AYNI_OK);
        work.node().step(1);
        const auto [kept, fresh] =
            ayni::kernel::kept_and_new_due_search(work.kernel(), work.node().sim().now_us());
        ASSERT_EQ(kept, fresh) << round;
    }
    EXPECT_EQ(resource().quarantine_id, 0U);
    EXPECT_EQ(work.goal(waiting).occurrences, 1U);
}
// ADR-0084: a fixed control envelope does not exempt other Scene-constrained
// list fields. One later out-of-Scene item rejects the entire physical request.
TEST_F(ManagedExecution, scene_covers_every_typed_list_item_before_any_permit_or_output) {
    typed_scene_list = true;
    start();
    create();
    pump(30);
    EXPECT_EQ(executor.submissions(), 0U);
    EXPECT_EQ(executor.samples(), 0U);
    EXPECT_EQ(work.task(task_id).hold_reason, AYNI_REASON_EXECUTION_SCENE);
}

// ADR-0084: lists remain supported when the same enabled Scene covers all rows
// and every control envelope source is proven fixed at catalog close.
TEST_F(ManagedExecution, one_scene_can_admit_all_items_beside_fixed_typed_control_fields) {
    typed_scene_list = true;
    second_scene_item = 8;
    start();
    create();
    pump(30);
    EXPECT_EQ(executor.submissions(), 1U);
    EXPECT_GT(executor.samples(), 0U);
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
// ADR-0052: an unanswered challenge is not work. Nothing reaches a Provider
// before its answer, and the call then runs as with an immediate executor.
TEST_F(ManagedExecution, held_challenge_waits_at_the_barrier_without_spinning) {
    answers = Answers::held;
    start();
    const auto goal = create();
    for (unsigned round = 0; round < 100 && executor.held(AYNI_EXECUTOR_CHALLENGE) == 0; ++round) {
        pump(1);
    }
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_CHALLENGE), 1U);
    for (unsigned at = 0; at < 5; ++at) {
        EXPECT_EQ(work.node().step().more_ready, 0U) << at;
    }
    EXPECT_EQ(executor.held(AYNI_EXECUTOR_CHALLENGE), 1U);
    EXPECT_EQ(executor.submissions(), 0U);
    EXPECT_EQ(gate().phase, AYNI_CONTROL_IDLE);
    ASSERT_EQ(executor.answer(), AYNI_OK);
    work.node().step();
    EXPECT_EQ(executor.submissions(), 1U);
    serve(40);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// ADR-0052: a challenge unanswered within the poll period ends the call before
// any permit, and its late answer is refused.
TEST_F(ManagedExecution, unanswered_challenge_times_out_before_any_permit) {
    answers = Answers::held;
    start();
    create();
    for (unsigned round = 0; round < 100 && executor.held(AYNI_EXECUTOR_CHALLENGE) == 0; ++round) {
        pump(1);
    }
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_CHALLENGE), 1U);
    pump(3);
    EXPECT_EQ(executor.answer(), AYNI_INVALID_STATE);
    serve(20);
    EXPECT_EQ(executor.submissions(), 0U);
    EXPECT_EQ(executor.samples(), 0U);
    EXPECT_EQ(gate().phase, AYNI_CONTROL_IDLE);
    EXPECT_EQ(resource().used, 0U);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// ADR-0052 / C-PERMIT-005: with the link lost, unanswered reads stop Core's
// renewals, the executor's own lease ends the output, and the quarantine
// stays until a measured stop reaches Core.
TEST_F(ManagedExecution, lost_link_ends_output_by_the_executors_own_lease) {
    answers = Answers::held;
    start(AYNI_FORM_SESSION, 200);
    create();
    for (unsigned round = 0; round < 100 && executor.submissions() == 0; ++round) {
        serve(1);
    }
    ASSERT_EQ(executor.submissions(), 1U);
    serve(10);
    const auto renewals = executor.renewals();
    EXPECT_GT(renewals, 0U);
    const uint64_t lost_at = now;
    for (unsigned round = 0; round < 30; ++round) {
        executor.drop_requests();
        pump(1);
    }
    EXPECT_EQ(executor.renewals(), renewals);
    EXPECT_EQ(gate().phase, AYNI_CONTROL_STOPPED);
    EXPECT_EQ(gate().stop_cause, AYNI_CONTROL_STOP_EXPIRED);
    EXPECT_LE(gate().stop_confirmed_at_us, lost_at + lease_us + tick_us);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_DISPATCHING);
    EXPECT_EQ(execution().executor_status, AYNI_RESULT_UNKNOWN);
    EXPECT_EQ(resource().quarantine_id, executor.logical_call());
    serve(20);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_EQ(execution().stop_cause, AYNI_CONTROL_STOP_EXPIRED);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// ADR-0052: a read can answer NOT_FOUND while the permit is still on its way
// to the gate. The answer fences the permit, so it never starts afterwards.
TEST_F(ManagedExecution, not_found_fences_a_permit_still_on_its_way_to_the_gate) {
    answers = Answers::held;
    permit_start = PermitStart::at_advance;
    start();
    create();
    for (unsigned round = 0; round < 100 && executor.submissions() == 0; ++round) {
        static_cast<void>(executor.answer());
        step_core();
    }
    ASSERT_EQ(executor.submissions(), 1U);
    for (unsigned round = 0; round < 10 && executor.held(AYNI_EXECUTOR_STATUS) == 0; ++round) {
        step_core();
    }
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_STATUS), 1U);
    ASSERT_EQ(executor.answer(), AYNI_OK);  // NOT_FOUND: the permit has not reached the gate.
    ASSERT_EQ(executor.advance(), AYNI_OK); // The fenced permit is refused; the Provider ends.
    EXPECT_EQ(executor.pending(), 0U);
    step_core(3);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_DISPATCHING);
    EXPECT_EQ(executor.held(AYNI_EXECUTOR_STATUS), 1U); // Asked again after the terminal.
    for (unsigned round = 0; round < 20 && resource().quarantine_id != 0U; ++round) {
        static_cast<void>(executor.answer());
        step_core();
    }
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_NOT_STARTED);
    EXPECT_EQ(execution().execution_ended, 1U);
    EXPECT_EQ(executor.samples(), 0U);
    EXPECT_EQ(gate().phase, AYNI_CONTROL_IDLE);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// ADR-0052: a NOT_FOUND read proves a permit never started only if it was
// sent after the Provider's terminal; an earlier read, answered later, is
// asked again. The executor's clock alone moves past the lease, so the
// forwarded permit is refused at the gate and the Provider ends.
TEST_F(ManagedExecution, not_found_proves_not_started_only_from_a_read_after_the_terminal) {
    answers = Answers::held;
    permit_start = PermitStart::at_advance;
    start();
    create();
    for (unsigned round = 0; round < 100 && executor.submissions() == 0; ++round) {
        static_cast<void>(executor.answer());
        step_core();
    }
    ASSERT_EQ(executor.submissions(), 1U);
    for (unsigned round = 0; round < 10 && executor.held(AYNI_EXECUTOR_STATUS) == 0; ++round) {
        step_core();
    }
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_STATUS), 1U);
    now += lease_us + 10 * tick_us;
    ASSERT_EQ(executor.advance(), AYNI_OK);
    ASSERT_EQ(executor.pending(), 0U);
    // No Core time passes: the terminal is taken while the read is pending,
    // and the stop that follows the terminal goes out.
    for (unsigned round = 0; round < 5 && executor.held(AYNI_EXECUTOR_STOP) == 0; ++round) {
        work.node().step();
    }
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_STOP), 1U);
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_STATUS), 1U);
    ASSERT_EQ(executor.answer(), AYNI_OK); // NOT_FOUND to the read sent before the terminal.
    for (unsigned round = 0; round < 5 && executor.held(AYNI_EXECUTOR_STATUS) == 0 &&
                             execution().phase == AYNI_EXECUTION_DISPATCHING;
         ++round) {
        work.node().step();
    }
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_DISPATCHING);
    EXPECT_EQ(executor.held(AYNI_EXECUTOR_STATUS), 1U);
    for (unsigned round = 0; round < 20 && resource().quarantine_id != 0U; ++round) {
        static_cast<void>(executor.answer());
        step_core();
    }
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_NOT_STARTED);
    EXPECT_EQ(executor.samples(), 0U);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// ADR-0052 / C-PERMIT-005: a renewal goes out only if the gate still holds
// when its challenge is answered, so a nonce issued after a revocation never
// renews the session; the stop does not wait for that challenge.
TEST_F(ManagedExecution, revocation_while_a_renewal_challenge_is_held_sends_no_renewal) {
    answers = Answers::held;
    start(AYNI_FORM_SESSION, 200);
    create();
    for (unsigned round = 0; round < 100 && executor.submissions() == 0; ++round) {
        serve(1);
    }
    ASSERT_EQ(executor.submissions(), 1U);
    for (unsigned round = 0; round < 20 && executor.held(AYNI_EXECUTOR_CHALLENGE) == 0; ++round) {
        serve(1);
    }
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_CHALLENGE), 1U);
    const auto renewals = executor.renewals();
    const ayni_grant_revoke_v1 body{sizeof(body), 0, grants[0]};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &body, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    work.node().step();
    EXPECT_EQ(executor.held(AYNI_EXECUTOR_STOP), 1U);
    ASSERT_EQ(executor.answer(), AYNI_OK); // The nonce is issued after the revocation.
    EXPECT_EQ(gate().phase, AYNI_CONTROL_STOPPING);
    for (unsigned round = 0; round < 5; ++round) {
        work.node().step();
        EXPECT_EQ(executor.held(AYNI_EXECUTOR_RENEW), 0U) << round;
    }
    serve(20);
    EXPECT_EQ(executor.renewals(), renewals);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
// ADR-0052 decision 3: a Host that steps only when more is ready or at the
// reported deadline. A stop sent while a status read is open still reports
// when its answer is due, and a stop left unanswered goes out again.
TEST_F(ManagedExecution, open_requests_wake_a_host_that_sleeps_until_the_reported_deadline) {
    answers = Answers::held;
    start(AYNI_FORM_SESSION, 200);
    create();
    for (unsigned round = 0; round < 100 && executor.submissions() == 0; ++round) {
        serve(1);
    }
    ASSERT_EQ(executor.submissions(), 1U);
    for (unsigned round = 0; round < 10 && executor.held(AYNI_EXECUTOR_STATUS) == 0; ++round) {
        step_core();
    }
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_STATUS), 1U);
    const ayni_grant_revoke_v1 body{sizeof(body), 0, grants[0]};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &body, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    auto report = work.node().step();
    for (unsigned round = 0; round < 10 && report.more_ready != 0U; ++round) {
        report = work.node().step();
    }
    ASSERT_EQ(report.more_ready, 0U);
    ASSERT_EQ(executor.held(AYNI_EXECUTOR_STOP), 1U);
    executor.drop_requests(); // The link is lost: nothing is answered.
    for (unsigned round = 0; round < 10 && executor.held(AYNI_EXECUTOR_STOP) == 0; ++round) {
        const uint64_t at = work.node().sim().now_us();
        ASSERT_NE(report.next_deadline_us, UINT64_MAX) << round;
        ASSERT_GT(report.next_deadline_us, at) << round;
        now += report.next_deadline_us - at;
        work.node().sim().advance_us(report.next_deadline_us - at);
        report = work.node().step();
        for (unsigned more = 0; more < 10 && report.more_ready != 0U; ++more) {
            report = work.node().step();
        }
    }
    EXPECT_EQ(executor.held(AYNI_EXECUTOR_STOP), 1U); // Sent again.
    EXPECT_EQ(execution().executor_status, AYNI_RESULT_UNKNOWN);
}
// ADR-0052: the first status read waits one poll period after the permit goes
// to the Provider, so a permit forwarded to the gate within it is not fenced
// by an early NOT_FOUND.
TEST_F(ManagedExecution, a_permit_forwarded_within_a_poll_period_starts) {
    permit_start = PermitStart::at_advance;
    start();
    const auto goal = create();
    wait_submission();
    step_core(); // Core runs a round ahead of the forwarded permit.
    pump(40);
    EXPECT_GT(executor.samples(), 0U);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(execution().phase, AYNI_EXECUTION_STOPPED);
    EXPECT_EQ(resource().quarantine_id, 0U);
}
} // namespace

// ADR-0060: White-box clone of a normally completed physical call exercises the erased codec;
// the actual Kernel/Mock action still follows its unchanged public stop chain.
TEST_F(ManagedExecution, erased_physical_record_preserves_independent_execution_facts) {
    start();
    ASSERT_NE(create(), 0U);
    wait_submission();
    pump(30);
    const auto &model = ayni::kernel::KernelTestProbe::work(work.kernel());
    uint32_t index = 0;
    ASSERT_TRUE(model.calls.find(executor.logical_call(), index));
    const auto before = model.calls.record(index);
    ASSERT_TRUE(before.state.execution.has_value());
    const ayni::work::CallTable::Limits limits{1, 1024, 1024, 1024,
                                               before.contract.business_contract.size};
    std::vector<uint64_t> storage(ayni::work::CallTable::storage_size(limits) / sizeof(uint64_t) +
                                  1);
    ayni::work::CallTable copy(storage.data(), limits);
    ASSERT_EQ(copy.restore_validated(before), AYNI_OK);
    ASSERT_TRUE(copy.find(before.identity.call_id, index));
    auto erased_at = before.state.latest;
    ++erased_at.transition.commit_seq;
    ++erased_at.transition.transition_id;
    ASSERT_EQ(copy.erase_content(index, erased_at), AYNI_OK);
    const auto erased = copy.record(index);
    ASSERT_TRUE(erased.state.content_erased.has_value());
    std::vector<uint8_t> bytes(ayni::work::call_record_size(erased));
    ayni::work::write_call_record(erased, bytes.data());
    ayni::work::CallRecord decoded{};
    ASSERT_TRUE(ayni::work::read_call_record(::ayni::test::codec_workspace(), bytes.data(),
                                             static_cast<uint32_t>(bytes.size()), decoded));
    ASSERT_TRUE(decoded.state.execution.has_value());
    EXPECT_EQ(decoded.state.execution->phase, before.state.execution->phase);
    EXPECT_EQ(decoded.state.execution->target, before.state.execution->target);
    EXPECT_EQ(decoded.state.execution->stop_cause, before.state.execution->stop_cause);
    EXPECT_EQ(decoded.state.execution->epochs.kernel_boot_epoch,
              before.state.execution->epochs.kernel_boot_epoch);
    EXPECT_EQ(decoded.state.outcome, before.state.outcome);
    EXPECT_EQ(decoded.input.size, 0U);
    EXPECT_EQ(decoded.result.size, 0U);
}
