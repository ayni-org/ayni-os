// ADR-0063: normal public-ABI Task subscriptions over deterministic Providers.
#ifndef AYNI_TESTS_UNIT_SUBSCRIPTION_FIXTURE_H
#define AYNI_TESTS_UNIT_SUBSCRIPTION_FIXTURE_H
#include "work_node.h"
#include <ayni/providers/mock/subscription.h>
namespace ayni::test {
class SubscriptionFixture : public testing::Test {
  protected:
    WorkNode work;
    ayni::providers::mock::SubscriptionProvider camera;
    ayni::providers::mock::Provider model;
    ayni_provider_v1 camera_ports{};
    struct Opening {
        ayni_subscription_open_v1 value;
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *completion;
        bool ended = false;
    };
    struct Query {
        uint64_t operation_id;
        ayni_attempt_ref_v1 attempt;
        ayni_provider_completion_t *completion;
        bool ended = false;
    };
    std::vector<Query> queries;
    bool minimal_journal = false;
    bool manual_camera = false;
    bool query_contract = false;
    std::vector<Opening> openings;
    std::vector<ayni_subscription_renew_v1> renewals;
    std::vector<uint64_t> grants;
    uint64_t command = 0;
    bool finish_analysis = false;
    bool close_requested = false;
    void SetUp() override {
        auto &config = work.node().config();
        config.call_capacity = 8;
        config.call_input_max_bytes = 128;
        config.call_result_max_bytes = 64;
        config.call_capability_capacity = 2;
        config.provider_capacity = 2;
        config.binding_capacity = 2;
        config.resource_capacity = 2;
        config.resource_request_capacity = 8;
        config.observation_source_capacity = 1;
        config.observation_ingress_capacity = 4;
        config.task_step_capacity = 8;
        config.storage_buffer_bytes = 0;
        if (minimal_journal) {
            config.storage_buffer_bytes = smallest_storage_buffer(config);
        }
        queries.reserve(16);
        openings.reserve(16);
        renewals.reserve(16);
        ayni::providers::mock::SubscriptionConfig capture;
        capture.clock_context = &work.node().sim();
        capture.now_us = [](void *context) {
            return static_cast<ayni::hosts::sim::Host *>(context)->now_us();
        };
        capture.frame_interval_us = 20000;
        capture.sample_ttl_us = 50000;
        ASSERT_EQ(camera.configure(capture), AYNI_OK);
        ASSERT_EQ(model.configure({ayni::providers::mock::Variant::a,
                                   ayni::providers::mock::Service::text,
                                   4,
                                   64,
                                   {}}),
                  AYNI_OK);
        work.node().on_create([this](ayni_kernel_t *kernel) { profile(kernel); });
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            return decide(snapshot, output);
        };
        work.run();
        for (const auto agent : {agent_a, agent_b}) {
            for (const auto capability : {"mock.observe", "mock.text"}) {
                const ayni_grant_create_v1 grant{sizeof(grant),
                                                 1,
                                                 text(capability),
                                                 work.instance(agent),
                                                 0,
                                                 AYNI_GRANT_PERMANENT,
                                                 0,
                                                 0,
                                                 {sizeof(ayni_scope_v1), 0, nullptr}};
                auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
                intent.command_id = ++command;
                const auto created = work.submit(intent);
                ASSERT_EQ(created.status, AYNI_OK);
                grants.push_back(created.result.object.id);
                pump(4);
            }
        }
        pump(20);
    }
    void TearDown() override {
        shutdown();
        EXPECT_EQ(work.node().stop_and_destroy(), AYNI_OK);
        EXPECT_EQ(camera.close(), AYNI_OK);
        EXPECT_EQ(model.close(), AYNI_OK);
    }
    void profile(ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        for (const auto id : {agent_a, agent_b}) {
            const auto agent = agent_desc(id);
            ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        }
        const auto strategy =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
        auto type = goal_type_desc("observe", 1);
        type.export_requirements = [](void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
            for (const auto capability : {"mock.observe", "mock.text"}) {
                const ayni_goal_requirement_v1 need{
                    sizeof(need), 1, text(capability), {sizeof(ayni_scope_v1), 0, nullptr}};
                const auto status = sink->emit(sink->ctx, &need);
                if (status != AYNI_OK) {
                    return status;
                }
            }
            return AYNI_OK;
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
        for (const auto capability : {"mock.observe", "mock.text"}) {
            const ayni_capability_desc_v1 authority{
                sizeof(authority), 1, text(capability), 0, 0, nullptr};
            ASSERT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
        }
        for (const auto resource : {"camera.slot", "model.slot"}) {
            const ayni_resource_desc_v1 description{
                sizeof(description), AYNI_RESOURCE_SHARED, text(resource), 2, 0, 0, 0, nullptr, {}};
            ASSERT_EQ(ayni_register_resource(kernel, &description), AYNI_OK);
            ASSERT_EQ(ayni_set_resource_health(kernel, text(resource), AYNI_RESOURCE_ONLINE),
                      AYNI_OK);
        }
        ayni_observation_source_v1 source{};
        ASSERT_EQ(camera.describe_source(source), AYNI_OK);
        ASSERT_EQ(ayni_register_observation_source(kernel, &source), AYNI_OK);
        ASSERT_EQ(camera.describe(camera_ports), AYNI_OK);
        auto capture = camera_ports;
        capture.ctx = this;
        capture.submit = [](void *context, const ayni_provider_request_v1 *request,
                            ayni_provider_completion_t *completion) {
            auto &self = *static_cast<SubscriptionFixture *>(context);
            self.openings.push_back({*request->subscription, request->attempt, completion});
            return self.manual_camera
                       ? AYNI_SUBMIT_ACCEPTED
                       : self.camera_ports.submit(self.camera_ports.ctx, request, completion);
        };
        capture.cancel = [](void *context, ayni_attempt_ref_v1 attempt) {
            auto &self = *static_cast<SubscriptionFixture *>(context);
            return self.manual_camera ? AYNI_SUBMIT_ACCEPTED
                                      : self.camera_ports.cancel(self.camera_ports.ctx, attempt);
        };
        capture.renew = [](void *context, const ayni_subscription_renew_v1 *request) {
            auto &self = *static_cast<SubscriptionFixture *>(context);
            self.renewals.push_back(*request);
            return self.manual_camera ? AYNI_SUBMIT_ACCEPTED
                                      : self.camera_ports.renew(self.camera_ports.ctx, request);
        };
        auto camera_capability = *capture.capabilities;
        if (query_contract) {
            camera_capability.contract.query = AYNI_CALL_QUERY_BEST_EFFORT;
            capture.query = [](void *context, uint64_t operation, ayni_attempt_ref_v1 attempt,
                               ayni_provider_completion_t *completion) {
                auto &self = *static_cast<SubscriptionFixture *>(context);
                self.queries.push_back({operation, attempt, completion});
                return AYNI_SUBMIT_ACCEPTED;
            };
        }
        capture.capabilities = &camera_capability;
        ayni_provider_v1 chat{};
        ASSERT_EQ(model.describe(chat), AYNI_OK);
        for (const auto *provider : {&capture, &chat}) {
            ASSERT_EQ(ayni_register_call_capability(kernel, provider->capabilities), AYNI_OK);
            ASSERT_EQ(ayni_register_provider(kernel, provider), AYNI_OK);
        }
        const ayni_binding_v1 bindings[]{{sizeof(ayni_binding_v1),
                                          1,
                                          text("camera"),
                                          text("mock.observe"),
                                          1,
                                          1,
                                          capture.provider_id,
                                          capture.endpoint_id,
                                          {}},
                                         {sizeof(ayni_binding_v1),
                                          1,
                                          text("model"),
                                          text("mock.text"),
                                          1,
                                          1,
                                          chat.provider_id,
                                          chat.endpoint_id,
                                          {}}};
        for (const auto &binding : bindings) {
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        }
    }
    ayni_status_t request(const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &output,
                          const char *step, const char *binding, ayni_policy_intent_kind_t kind) {
        ayni_call_request_v1 input{};
        input.struct_size = sizeof(input);
        input.kind = kind;
        input.step_key = text(step);
        if (binding) {
            input.binding_id = text(binding);
            input.binding_version = 1;
            input.arguments = {sizeof(ayni_scope_v1), 0, nullptr};
        }
        return output.request_call(output.call_context, &input);
    }
    // Leave a normal asynchronous model request pending while inspecting the
    // background lease; this does not require a subscription wait policy.
    ayni_status_t decide(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output) {
        ayni_subscription_info_v1 subscription{};
        subscription.struct_size = sizeof(subscription);
        const auto read =
            snapshot.subscription(snapshot.call_context, text("watch"), &subscription);
        if (read == AYNI_NOT_FOUND) {
            return request(snapshot, output, "watch", "camera", AYNI_POLICY_REQUEST_CALL);
        }
        if (read != AYNI_OK) {
            return read;
        }
        ayni_task_call_result_v1 analysis{};
        analysis.struct_size = sizeof(analysis);
        const auto result =
            snapshot.call_result(snapshot.call_context, text("analysis"), &analysis);
        if (result == AYNI_NOT_FOUND) {
            return request(snapshot, output, "analysis", "model", AYNI_POLICY_REQUEST_MODEL);
        }
        if (result != AYNI_OK) {
            return result;
        }
        if (finish_analysis || close_requested) {
            if (!close_requested) {
                close_requested = true;
                return request(snapshot, output, "watch", nullptr, AYNI_POLICY_CLOSE_SUBSCRIPTION);
            }
            return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, output);
        }
        // An orderly reboot may have cancelled the original analysis. A new
        // independently charged step keeps the Task live while collection reopens.
        return request(snapshot, output, "analysis.next", "model", AYNI_POLICY_REQUEST_MODEL);
    }
    void pump(uint32_t count = 1, uint64_t elapsed = 10000, bool capture = true,
              bool chat = false) {
        for (uint32_t i = 0; i < count; ++i) {
            work.node().sim().advance_us(elapsed);
            if (capture) {
                EXPECT_EQ(camera.advance(), AYNI_OK);
            }
            if (chat) {
                EXPECT_EQ(model.advance(), AYNI_OK);
            }
            work.node().step();
            work.node().sim().complete_storage();
        }
    }
    void shutdown() {
        if (work.node().lifecycle() == AYNI_LIFECYCLE_STOPPED) {
            return;
        }
        if (manual_camera) {
            for (auto &opening : openings) {
                if (!opening.ended) {
                    post(opening, AYNI_CALL_CANCELLED);
                }
            }
            for (auto &query : queries) {
                if (!query.ended) {
                    ayni_provider_result_v1 result{};
                    result.struct_size = sizeof(result);
                    result.attempt = query.attempt;
                    result.outcome = AYNI_CALL_ABANDONED;
                    result.status = AYNI_RESULT_UNKNOWN;
                    result.effect = AYNI_CALL_EFFECT_UNKNOWN;
                    result.query_result = AYNI_QUERY_RESULT_UNKNOWN;
                    EXPECT_EQ(ayni_provider_post(query.completion, &result), AYNI_OK);
                    query.ended = true;
                }
            }
        }
        ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
        for (uint32_t i = 0; i < 100 && work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED; ++i) {
            pump(1, 1000000, true, true);
        }
        EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    }
    void post(Opening &opening, ayni_call_outcome_t outcome, uint64_t sequence = 0) {
        ayni_provider_result_v1 result{};
        result.struct_size = sizeof(result);
        result.outcome = outcome;
        result.sequence = sequence;
        result.attempt = opening.attempt;
        result.status = outcome == AYNI_CALL_ABANDONED ? AYNI_RESULT_UNKNOWN : AYNI_OK;
        result.effect =
            outcome == AYNI_CALL_ABANDONED ? AYNI_CALL_EFFECT_UNKNOWN : AYNI_CALL_EFFECT_NONE;
        EXPECT_EQ(ayni_provider_post(opening.completion, &result), AYNI_OK);
        if (outcome == AYNI_CALL_ABANDONED || outcome == AYNI_CALL_CANCELLED) {
            opening.ended = true;
        }
    }
    uint64_t create(uint64_t agent = agent_a, uint32_t pause_group = 0) {
        auto body =
            WorkNode::create_body(work.instance(agent), 0, "observe", 1, {}, AYNI_TRIGGER_ONCE);
        body.pause_group = pause_group;
        const auto goal = work.create(body);
        EXPECT_EQ(goal.status, AYNI_OK);
        return goal.result.object.id;
    }
    uint64_t task(uint64_t goal) { return work.goal(goal).last_task_id; }
    ayni_subscription_info_v1 subscription(std::size_t opening = 0) {
        ayni_subscription_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_LT(opening, openings.size());
        EXPECT_EQ(ayni_get_subscription(
                      work.kernel(), openings[opening].value.subscription.subscription_id, &info),
                  AYNI_OK);
        return info;
    }
    ayni_observation_info_v1 observation(std::size_t opening = 0) {
        ayni_observation_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_subscription_observation(
                      work.kernel(), openings[opening].value.subscription.subscription_id, &info),
                  AYNI_OK);
        return info;
    }
    ayni_resource_info_v1 camera_resource() {
        ayni_resource_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_resource(work.kernel(), text("camera.slot"), &info), AYNI_OK);
        return info;
    }
};
} // namespace ayni::test
#endif
