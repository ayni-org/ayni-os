// ADR-0114: one generic runtime lifecycle serves unrelated software operation routes.
#include "work_node.h"
#include <array>
#include <ayni/authority.h>
#include <ayni/runtime/operation_session.h>
#include <ayni/workspace.hpp>
#include <gtest/gtest.h>
#include <optional>
#include <type_traits>

namespace {
using namespace ayni::test;
using Activity = ayni::runtime::OperationSession;
static_assert(!std::is_copy_constructible_v<Activity> && !std::is_move_constructible_v<Activity>);
class IndependentOperations : public testing::Test {
  protected:
    struct Pending {
        ayni_attempt_ref_v1 attempt{};
        ayni_provider_completion_t *completion = nullptr;
    };
    WorkNode work;
    Activity first, second;
    ayni::Workspace scratch;
    uint64_t command = 100, grants[2]{};
    uint32_t submissions = 0, cancels = 0;
    std::array<std::optional<Pending>, 2> pending;
    static constexpr const char *capabilities[]{"operation.alpha", "operation.beta"};
    void SetUp() override {
        ASSERT_EQ(scratch.create(ayni_value_workspace_requirements), AYNI_OK);
        auto &c = work.node().config();
        c.strategy_capacity = 4;
        c.goal_type_capacity = 8;
        c.goal_capacity = 8;
        c.task_capacity = 8;
        c.call_capacity = 8;
        c.call_capability_capacity = 4;
        c.capability_capacity = 4;
        c.provider_capacity = 2;
        c.binding_capacity = 4;
        c.resource_request_capacity = 8;
        c.grant_capacity = 8;
        c.goal_parameter_max_bytes = Activity::input_capacity;
        c.call_input_max_bytes = 128;
        c.call_result_max_bytes = 64;
        work.additional_registration = [&](ayni_kernel_t *kernel) { install(kernel); };
        work.run();
    }
    void TearDown() override {
        first.close_all();
        second.close_all();
        for (unsigned at = 0; at < 30; ++at) {
            step();
        }
    }
    void install(ayni_kernel_t *kernel) {
        ayni_schema_node_v1 input{}, result{};
        input.kind = AYNI_VALUE_I64;
        input.subtree_end = 1;
        input.min_i64 = 0;
        input.max_i64 = 100;
        result.kind = AYNI_VALUE_TEXT;
        result.subtree_end = 1;
        result.max_count = 64;
        const ayni_business_contract_v1 business{{&input, 1}, {&result, 1}, 0, 0, nullptr};
        std::array<ayni_call_capability_v1, 2> offers{};
        for (uint32_t at = 0; at < offers.size(); ++at) {
            const ayni_capability_desc_v1 authority{
                sizeof(authority), 1, text(capabilities[at]), 0, 0, nullptr};
            ASSERT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
            offers[at] = {sizeof(ayni_call_capability_v1),
                          1,
                          text(capabilities[at]),
                          {sizeof(ayni_call_contract_v1), AYNI_CALL_ACTION, AYNI_CALL_TOOL, 0,
                           AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST,
                           0, 10000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 64, 0,
                           nullptr, nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                          &business};
            ASSERT_EQ(ayni_register_call_capability(kernel, &offers[at]), AYNI_OK);
        }
        ayni_provider_v1 provider{};
        provider.struct_size = sizeof(provider);
        provider.version = 1;
        provider.provider_id = text("operation.provider");
        provider.endpoint_id = text("operation.endpoint");
        provider.ctx = this;
        provider.capability_count = uint32_t(offers.size());
        provider.capabilities = offers.data();
        provider.submit = [](void *ctx, const ayni_provider_request_v1 *request,
                             ayni_provider_completion_t *completion) {
            auto &self = *static_cast<IndependentOperations *>(ctx);
            const auto index = string_of(request->capability_id) == capabilities[0] ? 0U : 1U;
            if (self.pending[index]) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            ++self.submissions;
            self.pending[index] = Pending{request->attempt, completion};
            return AYNI_SUBMIT_ACCEPTED;
        };
        provider.cancel = [](void *ctx, ayni_attempt_ref_v1 attempt) {
            auto &self = *static_cast<IndependentOperations *>(ctx);
            for (auto &retained : self.pending) {
                if (retained && retained->attempt.boot_epoch == attempt.boot_epoch &&
                    retained->attempt.request_id == attempt.request_id) {
                    ++self.cancels;
                    ayni_provider_result_v1 completion_result{};
                    completion_result.struct_size = sizeof(completion_result);
                    completion_result.attempt = attempt;
                    completion_result.outcome = AYNI_CALL_CANCELLED;
                    completion_result.status = AYNI_OK;
                    completion_result.effect = AYNI_CALL_EFFECT_NONE;
                    const auto code = ayni_provider_post(retained->completion, &completion_result);
                    if (code == AYNI_OK || code == AYNI_INVALID_STATE) {
                        retained.reset();
                    }
                    return AYNI_SUBMIT_ACCEPTED;
                }
            }
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        };
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        for (uint32_t at = 0; at < offers.size(); ++at) {
            const ayni_binding_v1 binding{sizeof(binding),
                                          1,
                                          text(capabilities[at]),
                                          text(capabilities[at]),
                                          1,
                                          1,
                                          provider.provider_id,
                                          provider.endpoint_id,
                                          {},
                                          0,
                                          0,
                                          nullptr};
            ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
            Activity::Config config;
            config.goal_type = text(at ? "operation.goal.beta" : "operation.goal.alpha");
            config.strategy_id = text(at ? "operation.strategy.beta" : "operation.strategy.alpha");
            config.binding_id = binding.binding_id;
            config.policy_context = const_cast<char *>(capabilities[at]);
            config.requirements = [](void *ctx, ayni_bytes_v1,
                                     const ayni_goal_requirement_sink_v1 *sink) {
                const ayni_goal_requirement_v1 need{sizeof(need),
                                                    1,
                                                    text(static_cast<const char *>(ctx)),
                                                    {sizeof(ayni_scope_v1), 0, nullptr}};
                return sink->emit(sink->ctx, &need);
            };
            config.workspace = scratch.get();
            config.max_lifetime_us = 1000000;
            config.commands.context = this;
            config.commands.submit = [](void *ctx, ayni_intent_kind_t kind, const void *body,
                                        uint64_t *id, uint32_t revision) {
                auto &self = *static_cast<IndependentOperations *>(ctx);
                auto intent = self.work.intent(kind, body, revision);
                intent.command_id = ++self.command;
                const auto submitted = self.work.submit(intent);
                if (submitted.status == AYNI_OK && id) {
                    *id = submitted.result.object.id;
                }
                return Activity::SubmitResult{submitted.status, submitted.error.reason};
            };
            ASSERT_EQ((at ? second : first).install(kernel, config), AYNI_OK);
        }
    }
    void step(uint64_t us = 1000) {
        work.node().sim().advance_us(us);
        work.node().sim().complete_storage();
        work.node().step();
        const auto durable = work.node().info().durable_seq;
        EXPECT_EQ(first.advance(durable, work.node().sim().now_us()), AYNI_OK);
        EXPECT_EQ(second.advance(durable, work.node().sim().now_us()), AYNI_OK);
    }
    template <class Done> bool until(Done done) {
        for (unsigned at = 0; at < 200; ++at) {
            if (done()) {
                return true;
            }
            step();
        }
        const auto a = first.snapshot(), b = second.snapshot();
        std::printf("activity timeout first=%u status=%ld reason=%lu goal=%llu task=%llu second=%u "
                    "status=%ld reason=%lu\n",
                    unsigned(a.phase), long(a.status), (unsigned long)a.reason,
                    (unsigned long long)a.handle.goal, (unsigned long long)a.task,
                    unsigned(b.phase), long(b.status), (unsigned long)b.reason);
        return done();
    }
    void grant(uint32_t index) {
        const ayni_grant_create_v1 grant{sizeof(grant),
                                         1,
                                         text(capabilities[index]),
                                         work.instance(agent_a),
                                         0,
                                         AYNI_GRANT_PERMANENT,
                                         0,
                                         0,
                                         {sizeof(ayni_scope_v1), 0, nullptr}};
        auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
        intent.command_id = ++command;
        const auto result = work.submit(intent);
        ASSERT_EQ(result.status, AYNI_OK);
        grants[index] = result.result.object.id;
        step(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
        step(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
    }
    ayni_status_t open(Activity &session, uint64_t lifetime = 1000000) {
        ayni_value_node_v1 value{};
        value.kind = AYNI_VALUE_I64;
        value.subtree_end = 1;
        value.value.integer = 40;
        return session.open(work.node().info().boot_epoch, work.instance(agent_a), {&value, 1},
                            lifetime);
    }
};
constexpr const char *IndependentOperations::capabilities[];

TEST_F(IndependentOperations,
       ActivationHandoffIsRequiredAndUnrelatedGoalCancellationDoesNotStopSession) {
    // ADR-0114: handle delivery is the activation boundary; source-turn lifecycle is independent.
    grant(0);
    ASSERT_EQ(open(first), AYNI_OK);
    ASSERT_TRUE(
        until([&] { return first.snapshot().phase == Activity::Phase::ready_to_activate; }));
    const auto handle = first.snapshot().handle;
    for (unsigned at = 0; at < 5; ++at) {
        step();
    }
    EXPECT_EQ(submissions, 0U);
    auto stale = handle;
    ++stale.generation;
    EXPECT_EQ(first.activate(stale, work.node().sim().now_us()), AYNI_STALE);
    ASSERT_EQ(first.activate(handle, work.node().sim().now_us()), AYNI_OK);
    ASSERT_TRUE(until([&] { return submissions == 1; }));
    const auto peer = work.create(WorkNode::create_body(work.instance(agent_a)));
    ASSERT_EQ(peer.status, AYNI_OK);
    for (unsigned at = 0; at < 4; ++at) {
        step();
    }
    const auto info = work.goal(peer.result.object.id);
    const ayni_goal_target_v1 target{sizeof(target), 0, info.goal_id};
    auto intent = work.intent(AYNI_INTENT_GOAL_CANCEL, &target, info.revision);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    step();
    EXPECT_EQ(first.snapshot().phase, Activity::Phase::active);
    EXPECT_EQ(cancels, 0U);
    ASSERT_EQ(first.close(handle), AYNI_OK);
    EXPECT_EQ(first.release(), AYNI_BUSY);
    ASSERT_TRUE(until([&] { return first.snapshot().phase == Activity::Phase::terminal; }));
    EXPECT_EQ(cancels, 1U);
    ASSERT_EQ(first.release(), AYNI_OK);
    EXPECT_EQ(first.close(handle), AYNI_STALE);
}
TEST_F(IndependentOperations,
       MissingIndependentGrantAndCancelledOpeningNeverDispatchUnderlyingCall) {
    // ADR-0114: same Agent identity does not create authority; no automatic approval/detach.
    ASSERT_EQ(open(first), AYNI_OK);
    ASSERT_TRUE(until([&] { return first.snapshot().phase == Activity::Phase::terminal; }));
    EXPECT_EQ(first.snapshot().status, AYNI_DENIED);
    EXPECT_EQ(submissions, 0U);
    ASSERT_EQ(first.release(), AYNI_OK);
    grant(0);
    ASSERT_EQ(open(first), AYNI_OK);
    ASSERT_TRUE(
        until([&] { return first.snapshot().phase == Activity::Phase::ready_to_activate; }));
    first.close_all();
    ASSERT_TRUE(until([&] { return first.snapshot().phase == Activity::Phase::terminal; }));
    EXPECT_EQ(submissions, 0U);
}
TEST_F(IndependentOperations,
       TwoUnrelatedBindingsUseSameLifecycleAndRevocationStopsOnlySelectedSession) {
    // ADR-0114: the mechanism has no device/driver or conversation-type branch.
    grant(0);
    grant(1);
    ASSERT_EQ(open(first), AYNI_OK);
    ASSERT_EQ(open(second), AYNI_OK);
    ASSERT_TRUE(until([&] {
        return first.snapshot().phase == Activity::Phase::ready_to_activate &&
               second.snapshot().phase == Activity::Phase::ready_to_activate;
    }));
    ASSERT_EQ(first.activate(first.snapshot().handle, work.node().sim().now_us()), AYNI_OK);
    ASSERT_EQ(second.activate(second.snapshot().handle, work.node().sim().now_us()), AYNI_OK);
    ASSERT_TRUE(until([&] { return submissions == 2; }));
    ayni_grant_info_v1 grant{};
    grant.struct_size = sizeof(grant);
    ASSERT_EQ(ayni_get_grant(work.kernel(), grants[0], &grant), AYNI_OK);
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, grants[0]};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &revoke, grant.revision);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    ASSERT_TRUE(until([&] { return first.snapshot().phase == Activity::Phase::terminal; }));
    EXPECT_EQ(second.snapshot().phase, Activity::Phase::active);
    EXPECT_EQ(cancels, 1U);
}
TEST_F(IndependentOperations, OriginalProviderFailureSurvivesTaskAndGoalCompletion) {
    // ADR-0114: a terminal Goal must preserve the classified original Call failure.
    grant(0);
    ASSERT_EQ(open(first), AYNI_OK);
    ASSERT_TRUE(
        until([&] { return first.snapshot().phase == Activity::Phase::ready_to_activate; }));
    ASSERT_EQ(first.activate(first.snapshot().handle, work.node().sim().now_us()), AYNI_OK);
    ASSERT_TRUE(until([&] { return submissions == 1; }));
    auto result = ayni_provider_result_v1{};
    result.struct_size = sizeof(result);
    result.attempt = pending[0]->attempt;
    result.outcome = AYNI_CALL_FAILED;
    result.status = AYNI_UNSUPPORTED;
    result.effect = AYNI_CALL_EFFECT_NONE;
    ASSERT_EQ(ayni_provider_post(pending[0]->completion, &result), AYNI_OK);
    pending[0].reset();
    ASSERT_TRUE(until([&] { return first.snapshot().phase == Activity::Phase::terminal; }));
    EXPECT_EQ(first.snapshot().status, AYNI_UNSUPPORTED);
}
TEST_F(IndependentOperations, ExpiryBeforeActivationNeverDispatchesAndReportsExpired) {
    // ADR-0114: the finite independent Goal lifetime includes handle handoff time.
    grant(0);
    ASSERT_EQ(open(first, 100000), AYNI_OK);
    ASSERT_TRUE(
        until([&] { return first.snapshot().phase == Activity::Phase::ready_to_activate; }));
    step(200000);
    ASSERT_TRUE(until([&] { return first.snapshot().phase == Activity::Phase::terminal; }));
    EXPECT_EQ(submissions, 0U);
    EXPECT_EQ(first.snapshot().status, AYNI_EXPIRED);
}
TEST_F(IndependentOperations, ActivationRejectsElapsedDeadlineWithoutAnotherKernelStep) {
    // ADR-0114: a delayed owner cannot activate a stale ACTIVE snapshot after its absolute TTL.
    grant(0);
    ASSERT_EQ(open(first), AYNI_OK);
    ASSERT_TRUE(
        until([&] { return first.snapshot().phase == Activity::Phase::ready_to_activate; }));
    const auto snapshot = first.snapshot();
    work.node().sim().advance_us(snapshot.expires_at_us - work.node().sim().now_us());
    EXPECT_EQ(first.activate(snapshot.handle, work.node().sim().now_us()), AYNI_EXPIRED);
    EXPECT_EQ(submissions, 0U);
    ASSERT_TRUE(until([&] { return first.snapshot().phase == Activity::Phase::terminal; }));
}
} // namespace
