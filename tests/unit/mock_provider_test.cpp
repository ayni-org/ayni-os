// MA-008A/ADR-0048: reusable providers exercise only public registration, calls and completions.
#include "work_node.h"
#include <ayni/providers/mock.h>

namespace {
using namespace ayni::test;
using ayni::providers::mock::Provider;
using ayni::providers::mock::Service;
using ayni::providers::mock::Variant;
struct Profile {
    Variant variant;
    Service service;
};

class MockProvider : public ::testing::TestWithParam<Profile> {
  protected:
    void SetUp() override {
        ASSERT_EQ(provider.configure({GetParam().variant, GetParam().service, 1, 64}), AYNI_OK);
        ASSERT_EQ(provider.describe(descriptor), AYNI_OK);
        auto &config = work.node().config();
        config.call_capacity = 4;
        config.storage_buffer_bytes = 0;
        config.call_capability_capacity = 1;
        config.provider_capacity = 1;
        config.binding_capacity = 1;
        config.call_input_max_bytes = 128;
        config.call_result_max_bytes = 64;
        config.resource_capacity = 1;
        config.resource_request_capacity = 4;
        work.node().on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            ayni_task_call_result_v1 result{};
            result.struct_size = sizeof(result);
            const auto found = snapshot.call_result(snapshot.call_context, text("echo"), &result);
            if (found == AYNI_OK) {
                reply = string_of(result.text);
                return report(result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                    : AYNI_POLICY_REPORT_FAILURE,
                              snapshot, output);
            }
            if (found != AYNI_NOT_FOUND) {
                return found;
            }
            char input[] = "hello";
            const ayni_call_request_v1 request{sizeof(request),
                                               GetParam().service == Service::text
                                                   ? AYNI_POLICY_REQUEST_MODEL
                                                   : AYNI_POLICY_REQUEST_CALL,
                                               1,
                                               0,
                                               text("echo"),
                                               text("mock.selected"),
                                               {sizeof(ayni_scope_v1), 0, nullptr},
                                               {input, 5}};
            const auto status = output.request_call(output.call_context, &request);
            input[0] = 'x';
            return status;
        };
        work.run();
        for (const auto agent : {agent_a, agent_b}) {
            const ayni_grant_create_v1 grant{sizeof(grant),
                                             1,
                                             descriptor.capabilities->capability_id,
                                             work.instance(agent),
                                             0,
                                             AYNI_GRANT_PERMANENT,
                                             0,
                                             0,
                                             {sizeof(ayni_scope_v1), 0, nullptr}};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
            intent.command_id = agent;
            ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        }
        pump_kernel(12);
    }
    void TearDown() override {
        if (work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED) {
            shutdown();
        }
        EXPECT_EQ(provider.pending(), 0U);
        ASSERT_EQ(work.node().destroy(), AYNI_OK);
        EXPECT_EQ(provider.close(), AYNI_OK);
    }
    void shutdown() {
        ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
        pump_kernel(1); // Let Core request cancellation before advancing the executor.
        for (unsigned turn = 0; turn < 100 && work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED;
             ++turn) {
            ASSERT_EQ(provider.advance(), AYNI_OK);
            pump_kernel(1);
        }
        EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    }
    void register_profile(ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        for (const auto id : {agent_a, agent_b}) {
            const auto agent = agent_desc(id);
            ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        }
        const auto strategy =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
        auto goal_type = goal_type_desc("echo", 1, nullptr, this);
        goal_type.export_requirements = [](void *context, ayni_bytes_v1,
                                           const ayni_goal_requirement_sink_v1 *sink) {
            const auto &self = *static_cast<MockProvider *>(context);
            const ayni_goal_requirement_v1 required{sizeof(required),
                                                    1,
                                                    self.descriptor.capabilities->capability_id,
                                                    {sizeof(ayni_scope_v1), 0, nullptr}};
            return sink->emit(sink->ctx, &required);
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &goal_type), AYNI_OK);
        const auto &offer = *descriptor.capabilities;
        const ayni_capability_desc_v1 authority{
            sizeof(authority), 1, offer.capability_id, 0, 0, nullptr};
        ASSERT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
        // Deliberately two Core units against one Provider slot to exercise NOT_RETAINED.
        const ayni_resource_desc_v1 resource{sizeof(resource),
                                             AYNI_RESOURCE_SHARED,
                                             offer.contract.resources[0].resource_id,
                                             2,
                                             0,
                                             0,
                                             0,
                                             nullptr,
                                             {}};
        ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        ASSERT_EQ(ayni_register_call_capability(kernel, &offer), AYNI_OK);
        ASSERT_EQ(ayni_register_provider(kernel, &descriptor), AYNI_OK);
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("mock.selected"),
                                      offer.capability_id,
                                      1,
                                      1,
                                      descriptor.provider_id,
                                      descriptor.endpoint_id,
                                      {}};
        ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
    }
    void pump_kernel(unsigned turns) {
        for (unsigned at = 0; at < turns; ++at) {
            work.node().sim().complete_storage();
            // Pass the storage write-frequency interval before completing another barrier.
            work.node().sim().advance_us(10000);
            work.node().step();
        }
    }
    void drive(unsigned turns) {
        for (unsigned at = 0; at < turns; ++at) {
            ASSERT_EQ(provider.advance(), AYNI_OK);
            pump_kernel(1);
        }
    }
    uint64_t create(uint64_t agent = agent_a) {
        const auto created = work.create(
            WorkNode::create_body(work.instance(agent), 0, "echo", 1, {}, AYNI_TRIGGER_ONCE));
        EXPECT_EQ(created.status, AYNI_OK);
        return created.result.object.id;
    }
    Provider provider;
    WorkNode work; // Destroyed before the Provider context if a test exits early.
    ayni_provider_v1 descriptor{};
    std::string reply;
};

TEST_P(MockProvider, deferred_completion_copies_text_and_reports_action_progress) {
    const auto goal = create();
    pump_kernel(20);
    ASSERT_EQ(provider.pending(), 1U);
    EXPECT_EQ(provider.executions(), 0U);
    const auto task = work.goal(goal).last_task_id;
    const auto call = work.task(task).logical_call_id;
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_RUNNING);
    const auto ticks = GetParam().variant == Variant::a ? 1U : 2U;
    for (unsigned at = 0; at < ticks; ++at) {
        ASSERT_EQ(provider.advance(), AYNI_OK);
    }
    pump_kernel(2);
    if (GetParam().service == Service::action) {
        ayni_call_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_call(work.kernel(), call, &info), AYNI_OK);
        EXPECT_EQ(info.progress_sequence, 1U);
        EXPECT_EQ(info.outcome, 0U);
        EXPECT_EQ(provider.executions(), 0U);
    }
    drive(20);
    EXPECT_EQ(reply, "hello");
    EXPECT_EQ(provider.executions(), 1U);
    EXPECT_EQ(provider.pending(), 0U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}

TEST_P(MockProvider, cancellation_drains_the_borrow_and_close_refuses_while_pending) {
    const auto goal = create();
    pump_kernel(20);
    ASSERT_EQ(provider.pending(), 1U);
    EXPECT_EQ(provider.close(), AYNI_BUSY);
    const auto task = work.goal(goal).last_task_id;
    ASSERT_EQ(work.cancel_task(task).status, AYNI_OK);
    pump_kernel(5);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLING);
    drive(20);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(provider.executions(), 0U);
}

TEST_P(MockProvider, full_provider_queue_rejects_without_discarding_the_first_request) {
    const auto first = create();
    pump_kernel(20);
    ASSERT_EQ(provider.pending(), 1U);
    const auto second = create(agent_b);
    pump_kernel(20);
    EXPECT_EQ(provider.pending(), 1U);
    EXPECT_EQ(work.task(work.goal(second).last_task_id).lifecycle, AYNI_TASK_FAILED);
    drive(20);
    EXPECT_EQ(work.task(work.goal(first).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(provider.executions(), 1U);
}

TEST_P(MockProvider, normal_shutdown_cancels_pending_work_and_returns_every_borrow) {
    create();
    pump_kernel(20);
    ASSERT_EQ(provider.pending(), 1U);
    shutdown();
    EXPECT_EQ(provider.pending(), 0U);
    EXPECT_EQ(provider.executions(), 0U);
}

INSTANTIATE_TEST_SUITE_P(
    Variants, MockProvider,
    ::testing::Values(Profile{Variant::a, Service::text}, Profile{Variant::b, Service::text},
                      Profile{Variant::a, Service::action}, Profile{Variant::b, Service::action}),
    [](const ::testing::TestParamInfo<Profile> &profile_case) {
        return std::string(profile_case.param.variant == Variant::a ? "A" : "B") +
               (profile_case.param.service == Service::text ? "Text" : "Action");
    });

TEST(MockProviderConfig, invalid_bounds_and_unconfigured_operations_fail_explicitly) {
    Provider provider;
    ayni_provider_v1 descriptor{};
    descriptor.version = 7;
    EXPECT_EQ(provider.describe(descriptor), AYNI_INVALID_STATE);
    EXPECT_EQ(descriptor.version, 7U);
    EXPECT_EQ(provider.advance(), AYNI_INVALID_STATE);
    EXPECT_EQ(provider.close(), AYNI_INVALID_STATE);
    EXPECT_EQ(provider.configure({Variant::a, Service::text, 0, 64}), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(provider.configure({Variant::a, Service::text, 65, 64}), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(provider.configure({Variant::a, Service::text, 1, 0}), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(provider.configure({Variant::a, Service::text, 1, 4097}), AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(provider.configure({Variant::b, Service::action, 1, 64}), AYNI_OK);
    EXPECT_EQ(provider.configure({}), AYNI_INVALID_STATE);
    ASSERT_EQ(provider.describe(descriptor), AYNI_OK);
    EXPECT_EQ(string_of(descriptor.provider_id), "mock.b.present");
    EXPECT_EQ(descriptor.capabilities->contract.query, AYNI_CALL_QUERY_NONE);
    EXPECT_EQ(provider.close(), AYNI_OK);
}
} // namespace
