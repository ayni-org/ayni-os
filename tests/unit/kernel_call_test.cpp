// Normal public ABI call scenarios, C-CALL-002/012, C-COMMIT-007 and ADR-0045.
// Sim storage timing is explicit; no fault injection or restart campaign.
#include "work_node.h"

#include <ayni/calls.h>
#include <ayni/memory.h>
#include <thread>

namespace {
using namespace ayni::test;

class ManagedCall : public ::testing::Test {
  protected:
    struct Pending {
        ayni_attempt_ref_v1 attempt;
        uint64_t call;
        uint64_t operation;
        ayni_provider_completion_t *completion;
        std::string text;
        bool ended = false;
    };
    void SetUp() override {
        auto &config = work.node().config();
        config.call_capacity = call_limit;
        config.task_step_capacity = step_limit;
        config.call_input_max_bytes = 128;
        config.call_result_max_bytes = 64;
        config.call_capability_capacity = 1;
        config.provider_capacity = 1;
        config.binding_capacity = 1;
        config.goal_type_capacity = 4;
        config.resource_capacity = 1;
        config.resource_request_capacity = 4;
        config.storage_buffer_bytes = 0;
        work.node().on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        work.strategy().answer = [this](const auto &snapshot, auto &output) {
            last_task = snapshot.task_id;
            ayni_task_call_result_v1 result{};
            result.struct_size = sizeof(result);
            const auto read = snapshot.call_result(snapshot.call_context, text("ask"), &result);
            if (read == AYNI_OK) {
                seen_result = string_of(result.text);
                seen_call = result.logical_call_id;
                return report(result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                                    : AYNI_POLICY_REPORT_FAILURE,
                              snapshot, output);
            }
            if (read != AYNI_NOT_FOUND) {
                return read;
            }
            char body[] = "hello";
            const ayni_call_request_v1 request{
                sizeof(request),   AYNI_POLICY_REQUEST_MODEL,           1,        0, text("ask"),
                text("mock.echo"), {sizeof(ayni_scope_v1), 0, nullptr}, {body, 5}};
            const auto status = output.request_call(output.call_context, &request);
            body[0] = 'x'; // Immediate copy must outlive these callback-local bytes.
            return status;
        };
        work.run();
        for (const auto agent : {agent_a, agent_b}) {
            const ayni_grant_create_v1 body{sizeof(body),
                                            1,
                                            text("echo"),
                                            work.instance(agent),
                                            0,
                                            AYNI_GRANT_PERMANENT,
                                            0,
                                            0,
                                            {sizeof(ayni_scope_v1), 0, nullptr}};
            auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &body);
            intent.command_id = ++command;
            const auto accepted = work.submit(intent);
            ASSERT_EQ(accepted.status, AYNI_OK);
            grants.push_back(accepted.result.object.id);
        }
        pump(20);
    }
    void TearDown() override {
        if (work.node().lifecycle() == AYNI_LIFECYCLE_STOPPED) {
            return;
        }
        cancel_posts_terminal = true;
        for (std::size_t at = 0; at < pending.size(); ++at) {
            if (!pending[at].ended) {
                post(at, AYNI_CALL_CANCELLED, AYNI_CALL_EFFECT_NONE, {});
            }
        }
        for (std::size_t at = 0; at < queries.size(); ++at) {
            if (!queries[at].ended) {
                post_query(at, AYNI_QUERY_RESULT_UNKNOWN, AYNI_CALL_ABANDONED,
                           AYNI_CALL_EFFECT_UNKNOWN, {});
            }
        }
        ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
        work.node().drain();
        EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    }
    void register_profile(ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        for (const auto agent : {agent_a, agent_b}) {
            const auto desc = agent_desc(agent);
            ASSERT_EQ(ayni_register_agent(kernel, &desc), AYNI_OK);
        }
        const auto strategy =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy());
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
        auto goal_type = goal_type_desc("chat", 1);
        goal_type.export_requirements = [](void *, ayni_bytes_v1,
                                           const ayni_goal_requirement_sink_v1 *sink) {
            const ayni_goal_requirement_v1 requirement{
                sizeof(requirement), 1, text("echo"), {sizeof(ayni_scope_v1), 0, nullptr}};
            return sink->emit(sink->ctx, &requirement);
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &goal_type), AYNI_OK);
        for (const auto profile :
             {AYNI_WORK_FOREGROUND, AYNI_WORK_REMINDER, AYNI_WORK_BACKGROUND}) {
            goal_type.goal_type_id = text(profile == AYNI_WORK_FOREGROUND ? "foreground"
                                          : profile == AYNI_WORK_REMINDER ? "reminder"
                                                                          : "background");
            goal_type.work_profile = {profile, 0, 0};
            goal_type.trigger_kinds = profile == AYNI_WORK_BACKGROUND
                                          ? AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL)
                                          : any_trigger;
            goal_type.occurrence_wait_max_us = profile == AYNI_WORK_BACKGROUND
                                                   ? AYNI_RESOURCE_WAIT_DEFAULT_US
                                                   : AYNI_OCCURRENCE_WAIT_UNBOUNDED;
            ASSERT_EQ(ayni_register_goal_type(kernel, &goal_type), AYNI_OK);
        }
        const ayni_capability_desc_v1 authority{sizeof(authority), 1, text("echo"), 0, 0, nullptr};
        ASSERT_EQ(ayni_register_capability(kernel, &authority), AYNI_OK);
        const ayni_resource_desc_v1 resource{
            sizeof(resource), AYNI_RESOURCE_SHARED, text("model.slot"), 1, 0, 0, 0, nullptr, {}};
        ASSERT_EQ(ayni_register_resource(kernel, &resource), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        const ayni_call_resource_v1 units{resource.resource_id, 1, 0};
        ayni_call_capability_v1 capability{};
        capability.struct_size = sizeof(capability);
        capability.version = 1;
        capability.capability_id = text("echo");
        capability.contract = {sizeof(capability.contract),
                               AYNI_CALL_UNARY,
                               AYNI_CALL_MODEL,
                               0,
                               AYNI_CALL_IDEMPOTENT,
                               query_authority,
                               AYNI_CALL_CANCEL_REQUEST,
                               0,
                               30000,
                               AYNI_CALL_INPUT_FIELDS_TEXT,
                               AYNI_CALL_RESULT_TEXT,
                               64,
                               64,
                               1,
                               &units,
                               nullptr};
        ASSERT_EQ(ayni_register_call_capability(kernel, &capability), AYNI_OK);
        ayni_provider_v1 provider{};
        provider.struct_size = sizeof(provider);
        provider.version = 1;
        provider.provider_id = text("mock");
        provider.endpoint_id = text("mock.endpoint");
        provider.ctx = this;
        provider.capability_count = 1;
        provider.capabilities = &capability;
        std::array<ayni_call_capability_v1, 2> offers{{capability, capability}};
        if (mixed_offers) {
            offers[1].capability_id = text("remote.echo");
            offers[1].contract.effects = AYNI_CALL_EFFECT_EGRESS;
            provider.destination_id = text("cloud");
            provider.capability_count = static_cast<uint32_t>(offers.size());
            provider.capabilities = offers.data();
        }
        provider.submit = [](void *ctx, const ayni_provider_request_v1 *request,
                             ayni_provider_completion_t *completion) {
            auto &self = *static_cast<ManagedCall *>(ctx);
            self.pending.push_back({request->attempt, request->logical_call_id,
                                    request->operation_id, completion, string_of(request->text),
                                    false});
            return AYNI_SUBMIT_ACCEPTED;
        };
        provider.cancel = [](void *ctx, ayni_attempt_ref_v1 attempt) {
            auto &self = *static_cast<ManagedCall *>(ctx);
            ++self.cancellations;
            for (std::size_t at = 0; at < self.pending.size(); ++at) {
                if (self.pending[at].attempt.request_id == attempt.request_id &&
                    !self.pending[at].ended && self.cancel_posts_terminal) {
                    self.post(at, AYNI_CALL_CANCELLED, AYNI_CALL_EFFECT_NONE, {});
                }
            }
            return AYNI_SUBMIT_ACCEPTED;
        };
        provider.query = [](void *ctx, uint64_t operation, ayni_attempt_ref_v1 attempt,
                            ayni_provider_completion_t *completion) {
            auto &self = *static_cast<ManagedCall *>(ctx);
            self.queries.push_back({attempt, 0, operation, completion, {}, false});
            return AYNI_SUBMIT_ACCEPTED;
        };
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("mock.echo"),
                                      capability.capability_id,
                                      1,
                                      1,
                                      provider.provider_id,
                                      provider.endpoint_id,
                                      {nullptr, 0}};
        ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
    }
    uint64_t create(uint64_t agent = agent_a) {
        const auto created = work.create(
            WorkNode::create_body(work.instance(agent), 0, "chat", 1, {}, AYNI_TRIGGER_ONCE));
        EXPECT_EQ(created.status, AYNI_OK);
        return created.result.object.id;
    }
    uint64_t create_profile(const char *type, uint64_t agent = agent_a, uint64_t period_us = 0) {
        auto body =
            WorkNode::create_body(work.instance(agent), 0, type, 1, {},
                                  period_us == 0 ? AYNI_TRIGGER_ONCE : AYNI_TRIGGER_INTERVAL);
        body.trigger_period_us = period_us;
        const auto created = work.create(body);
        EXPECT_EQ(created.status, AYNI_OK);
        return created.result.object.id;
    }
    void pump(unsigned rounds) {
        for (unsigned round = 0; round < rounds; ++round) {
            work.node().sim().complete_storage();
            work.node().sim().advance_us(10000);
            work.node().step();
            ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        }
    }
    void post(std::size_t at, ayni_call_outcome_t outcome, ayni_call_effect_t effect,
              ayni_string_v1 body) {
        auto &call = pending[at];
        const ayni_provider_result_v1 result{sizeof(result),
                                             outcome,
                                             call.attempt,
                                             0,
                                             outcome == AYNI_CALL_ABANDONED ? AYNI_RESULT_UNKNOWN
                                             : outcome == AYNI_CALL_FAILED  ? AYNI_NOT_READY
                                                                            : AYNI_OK,
                                             AYNI_REASON_NONE,
                                             effect,
                                             0,
                                             body,
                                             0,
                                             0,
                                             nullptr};
        EXPECT_EQ(ayni_provider_post(call.completion, &result), AYNI_OK);
        call.ended = true;
    }
    void post_query(std::size_t at, uint32_t conclusion, ayni_call_outcome_t outcome,
                    ayni_call_effect_t effect, ayni_string_v1 body) {
        auto &query = queries[at];
        const ayni_provider_result_v1 result{sizeof(result),
                                             outcome,
                                             query.attempt,
                                             0,
                                             outcome == AYNI_CALL_ABANDONED ? AYNI_RESULT_UNKNOWN
                                             : outcome == AYNI_CALL_FAILED  ? AYNI_NOT_READY
                                                                            : AYNI_OK,
                                             AYNI_REASON_NONE,
                                             effect,
                                             conclusion,
                                             body,
                                             0,
                                             0,
                                             nullptr};
        EXPECT_EQ(ayni_provider_post(query.completion, &result), AYNI_OK);
        query.ended = true;
    }
    auto query_call(uint64_t call_id, uint64_t command_id, uint32_t revision = 1,
                    uint64_t session = 0) {
        const ayni_call_query_result_v1 body{sizeof(body), 0, call_id};
        auto intent = work.intent(AYNI_INTENT_CALL_QUERY_RESULT, &body, revision);
        intent.command_id = command_id;
        if (session != 0U) {
            intent.session_id = session;
        }
        return work.submit(intent);
    }
    auto resume_wait(uint64_t task, uint64_t command_id, uint32_t revision, uint64_t session = 0) {
        const ayni_task_target_v1 body{sizeof(body), 0, task};
        auto intent = work.intent(AYNI_INTENT_TASK_RESUME_WAIT, &body, revision);
        intent.command_id = command_id;
        if (session != 0U) {
            intent.session_id = session;
        }
        return work.submit(intent);
    }
    uint64_t expire_resource_wait() {
        EXPECT_EQ(
            ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_OFFLINE),
            AYNI_OK);
        const auto goal = create();
        pump(8);
        work.node().sim().advance_us(30000000);
        pump(4);
        return goal;
    }
    ayni_call_info_v1 call_info(uint64_t id) {
        ayni_call_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_call(work.kernel(), id, &info), AYNI_OK);
        return info;
    }
    WorkNode work;
    std::vector<Pending> queries;
    std::vector<uint64_t> grants;
    uint32_t step_limit = 8;
    uint32_t call_limit = 4;
    bool mixed_offers = false;
    ayni_call_query_authority_t query_authority = AYNI_CALL_QUERY_AUTHORITATIVE;
    std::vector<Pending> pending;
    uint64_t last_task = 0;
    uint64_t seen_call = 0;
    uint64_t command = 0;
    std::string seen_result;
    unsigned cancellations = 0;
    bool cancel_posts_terminal = false;
};

TEST_F(ManagedCall, async_worker_result_advances_the_same_task_and_preserves_copied_input) {
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].text, "hello");
    const auto running = work.task(work.goal(goal).last_task_id);
    EXPECT_EQ(running.lifecycle, AYNI_TASK_RUNNING);
    EXPECT_EQ(running.logical_call_id, pending[0].call);
    EXPECT_EQ(running.model_calls_used, 1U);
    std::thread worker(
        [this] { post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("answer")); });
    worker.join();
    pump(12);
    EXPECT_EQ(work.task(running.task_id).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(seen_result, "answer");
    EXPECT_EQ(seen_call, pending[0].call);
}

TEST_F(ManagedCall, cancellation_waits_for_terminal_and_late_success_does_not_reopen_task) {
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto task = work.goal(goal).last_task_id;
    ASSERT_EQ(work.cancel_task(task).status, AYNI_OK);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLING);
    pump(4);
    EXPECT_EQ(cancellations, 1U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLING);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("late answer"));
    pump(12);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_TRUE(seen_result.empty());
    EXPECT_EQ(pending.size(), 1U);
}

TEST_F(ManagedCall, a_shared_model_slot_releases_to_the_other_agent_after_completion) {
    const auto first = create();
    const auto second = create(agent_b);
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("first"));
    pump(16);
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_NE(pending[0].call, pending[1].call);
    EXPECT_NE(pending[0].attempt.request_id, pending[1].attempt.request_id);
    post(1, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("second"));
    pump(16);
    EXPECT_EQ(work.goal(first).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(work.goal(second).lifecycle, AYNI_GOAL_SATISFIED);
}

TEST_F(ManagedCall, normal_shutdown_requests_cancel_and_waits_for_returned_borrow) {
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    work.node().step();
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_DRAINING);
    post(0, AYNI_CALL_CANCELLED, AYNI_CALL_EFFECT_NONE, {});
    work.node().drain();
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    // TearDown normally drives RUNNING; shutdown already completed here.
}
// C-COMMIT-006/007: external presentation waits for the result's durable batch.
TEST_F(ManagedCall, result_text_is_copied_out_only_after_its_required_sequence_is_durable) {
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("answer"));
    ayni_call_info_v1 info{};
    info.struct_size = sizeof(info);
    for (unsigned round = 0; round < 16; ++round) {
        work.node().step(1); // Do not acknowledge the newly queued storage write yet.
        ASSERT_EQ(ayni_get_call(work.kernel(), pending[0].call, &info), AYNI_OK);
        if (info.outcome == AYNI_CALL_COMPLETED) {
            break;
        }
    }
    ASSERT_EQ(info.outcome, AYNI_CALL_COMPLETED);
    char buffer[16]{};
    uint32_t size = 99;
    EXPECT_EQ(
        ayni_copy_call_result(work.kernel(), info.logical_call_id, buffer, sizeof(buffer), &size),
        AYNI_BUSY);
    EXPECT_EQ(size, 99U);
    pump(12);
    EXPECT_EQ(ayni_copy_call_result(work.kernel(), info.logical_call_id, buffer, 1, &size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, 6U);
    ASSERT_EQ(
        ayni_copy_call_result(work.kernel(), info.logical_call_id, buffer, sizeof(buffer), &size),
        AYNI_OK);
    EXPECT_EQ(std::string(buffer, size), "answer");
}

// C-CALL-008/012: local cancellation does not erase or reissue unknown effects.
TEST_F(ManagedCall, user_can_cancel_drained_unknown_work_while_the_call_remains_queryable) {
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    const auto task = work.goal(goal).last_task_id;
    EXPECT_EQ(work.task(task).hold_status, AYNI_RESULT_UNKNOWN);
    ASSERT_EQ(work.cancel_task(task).status, AYNI_OK);
    pump(8);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLED);
    ayni_call_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_call(work.kernel(), pending[0].call, &info), AYNI_OK);
    EXPECT_EQ(info.phase, AYNI_CALL_RESULT_UNKNOWN);
    EXPECT_EQ(info.effect, AYNI_CALL_EFFECT_UNKNOWN);
    EXPECT_EQ(pending.size(), 1U);
}
// C-RES-005: an ordinary resource wait records a pause, not silent loss.
TEST_F(ManagedCall, ordinary_wait_expiry_pauses_without_dispatching_the_queued_call) {
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_OFFLINE),
              AYNI_OK);
    const auto goal = create();
    pump(8);
    const auto task = work.goal(goal).last_task_id;
    const auto call = work.task(task).logical_call_id;
    ASSERT_NE(call, 0U);
    ayni_call_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_call(work.kernel(), call, &info), AYNI_OK);
    EXPECT_EQ(info.wait_reason, AYNI_CALL_WAIT_OFFLINE);
    work.node().sim().advance_us(30000000);
    pump(4);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task).hold_reason, AYNI_REASON_RESOURCE_WAIT);
    EXPECT_TRUE(pending.empty());
}

// ADR-0045: explicit owner resume preserves the accepted identity, counter and retry receipt.
TEST_F(ManagedCall, owner_resumes_the_same_resource_wait_without_another_model_charge) {
    const auto goal = expire_resource_wait();
    const auto task = work.goal(goal).last_task_id;
    const auto before = work.task(task);
    const auto call = before.logical_call_id;
    ASSERT_NE(call, 0U);
    ASSERT_EQ(before.hold_reason, AYNI_REASON_RESOURCE_WAIT);
    const auto operation = call_info(call).operation_id;
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    pump(8);
    EXPECT_TRUE(pending.empty());
    const auto request = ++command;
    EXPECT_EQ(
        resume_wait(task, request, before.revision, work.open_session(AYNI_CHANNEL_VOICE)).status,
        AYNI_DENIED);
    EXPECT_EQ(resume_wait(task, request, before.revision + 1).status, AYNI_REVISION_CONFLICT);
    const auto accepted = resume_wait(task, request, before.revision);
    ASSERT_EQ(accepted.status, AYNI_OK);
    EXPECT_EQ(resume_wait(task, request, before.revision).result.commit_seq,
              accepted.result.commit_seq);
    EXPECT_EQ(resume_wait(task, request, before.revision + 1).status, AYNI_IDEMPOTENCY_CONFLICT);
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].call, call);
    EXPECT_EQ(pending[0].operation, operation);
    EXPECT_EQ(work.task(task).model_calls_used, before.model_calls_used);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("answer"));
    pump(12);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(resume_wait(task, request, before.revision).result.commit_seq,
              accepted.result.commit_seq);
}

// ADR-0045: wait and user pause can be released in either order without losing the other.
TEST_F(ManagedCall, wait_resume_preserves_user_pause_and_ordinary_resume_preserves_wait) {
    const auto goal = expire_resource_wait();
    const auto task = work.goal(goal).last_task_id;
    const auto call = work.task(task).logical_call_id;
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal).status, AYNI_OK);
    EXPECT_EQ(work.task(task).hold_reason, AYNI_REASON_RESOURCE_WAIT);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    ASSERT_EQ(resume_wait(task, ++command, work.task(task).revision).status, AYNI_OK);
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    pump(8);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task).logical_call_id, call);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal).status, AYNI_OK);
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].call, call);
    EXPECT_EQ(work.task(task).model_calls_used, 1U);
}

// ADR-0045: cancelling a retained wait needs no Provider borrow or new execution.
TEST_F(ManagedCall, retained_resource_wait_can_be_cancelled_without_provider_dispatch) {
    const auto goal = expire_resource_wait();
    const auto task = work.goal(goal).last_task_id;
    const auto call = work.task(task).logical_call_id;
    ASSERT_EQ(work.cancel_task(task).status, AYNI_OK);
    pump(8);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(call_info(call).outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(resume_wait(task, ++command, work.task(task).revision).status, AYNI_INVALID_STATE);
}

// ADR-0045: owner resume does not restore a revoked execution Grant.
TEST_F(ManagedCall, wait_resume_never_bypasses_revoked_execution_authority) {
    const auto goal = expire_resource_wait();
    const auto task = work.goal(goal).last_task_id;
    const ayni_grant_revoke_v1 body{sizeof(body), 0, grants[0]};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &body, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    pump(8);
    ASSERT_EQ(resume_wait(task, ++command, work.task(task).revision).status, AYNI_OK);
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    pump(16);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(work.task(task).model_calls_used, 1U);
}

// ADR-0045: explicit read of the original operation survives execution revocation.
TEST_F(ManagedCall, owner_reconciles_after_grant_revocation_without_repeating_execution) {
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending[0].call;
    const auto task = work.goal(goal).last_task_id;
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, grants[0]};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &revoke, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    pump(12);
    const auto request = ++command;
    const auto accepted = query_call(call, request);
    ASSERT_EQ(accepted.status, AYNI_OK);
    EXPECT_EQ(accepted.result.object.revision, 2U);
    EXPECT_EQ(query_call(call, request).result.commit_seq, accepted.result.commit_seq);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    EXPECT_EQ(queries[0].operation, pending[0].operation);
    EXPECT_NE(queries[0].attempt.request_id, pending[0].attempt.request_id);
    post_query(0, AYNI_QUERY_RESULT_FOUND, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE,
               text("recorded answer"));
    pump(12);
    const auto info = call_info(call);
    EXPECT_EQ(info.phase, AYNI_CALL_FINISHED);
    EXPECT_EQ(info.query_phase, AYNI_QUERY_FINISHED);
    EXPECT_EQ(info.query_conclusion, AYNI_QUERY_RESULT_FOUND);
    EXPECT_EQ(work.task(task).lifecycle,
              AYNI_TASK_PAUSED); // Revocation still prevents further work.
    EXPECT_EQ(query_call(call, request).status, AYNI_OK);
    pump(4);
    EXPECT_EQ(queries.size(), 1U);
    EXPECT_EQ(pending.size(), 1U);
}

TEST_F(ManagedCall, reconciled_result_resumes_only_the_still_waiting_task) {
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    ASSERT_EQ(query_call(pending[0].call, ++command).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_FOUND, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE,
               text("found"));
    pump(12);
    EXPECT_EQ(work.task(work.goal(goal).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(seen_result, "found");
}

TEST_F(ManagedCall, a_cancelled_task_stays_cancelled_when_reconciliation_finds_success) {
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto task = work.goal(goal).last_task_id;
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    ASSERT_EQ(work.cancel_task(task).status, AYNI_OK);
    pump(8);
    ASSERT_EQ(query_call(pending[0].call, ++command).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_FOUND, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE,
               text("late fact"));
    pump(12);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(call_info(pending[0].call).outcome, AYNI_CALL_COMPLETED);
    EXPECT_TRUE(seen_result.empty());
}

TEST_F(ManagedCall, query_requires_management_channel_exact_identity_and_current_query_revision) {
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending[0].call;
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    const auto request = ++command;
    EXPECT_EQ(query_call(call, request, 1, work.open_session(AYNI_CHANNEL_VOICE)).status,
              AYNI_DENIED);
    EXPECT_EQ(query_call(call, request, 2).status, AYNI_REVISION_CONFLICT);
    ASSERT_EQ(query_call(call, request).status, AYNI_OK);
    EXPECT_EQ(query_call(call, request, 2).status, AYNI_IDEMPOTENCY_CONFLICT);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_UNKNOWN, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_RESULT_UNKNOWN);
    ASSERT_EQ(query_call(call, ++command, 2).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(queries.size(), 2U);
    EXPECT_NE(queries[0].attempt.request_id, queries[1].attempt.request_id);
    post_query(1, AYNI_QUERY_RESULT_NOT_ACCEPTED, AYNI_CALL_FAILED, AYNI_CALL_EFFECT_NONE, {});
    pump(12);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_FINISHED);
    EXPECT_EQ(pending.size(), 1U);
}

class BestEffortCall : public ManagedCall {
    void SetUp() override {
        query_authority = AYNI_CALL_QUERY_BEST_EFFORT;
        ManagedCall::SetUp();
    }
};

TEST_F(BestEffortCall, missing_record_never_proves_nonexecution) {
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending[0].call;
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    ASSERT_EQ(query_call(call, ++command).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_NOT_ACCEPTED, AYNI_CALL_FAILED, AYNI_CALL_EFFECT_NONE, {});
    pump(8);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_RESULT_UNKNOWN);
    EXPECT_EQ(call_info(call).query_conclusion, AYNI_QUERY_RESULT_UNKNOWN);
    EXPECT_EQ(pending.size(), 1U);
}

class ShortCallWorkflow : public ManagedCall {
    void SetUp() override {
        step_limit = 2;
        ManagedCall::SetUp();
    }
};

// C-POLICY-006/ADR-0045: accepted failed calls consume bounded step identities;
// completing only successful steps must not permit an unlimited failure loop.
TEST_F(ShortCallWorkflow, failed_calls_still_consume_the_task_step_capacity) {
    unsigned requests = 0;
    work.strategy().answer = [&](const auto &, auto &output) {
        const auto key = std::string("try.") + std::to_string(++requests);
        const ayni_call_request_v1 request{sizeof(request),
                                           AYNI_POLICY_REQUEST_MODEL,
                                           1,
                                           0,
                                           {key.data(), static_cast<uint32_t>(key.size())},
                                           text("mock.echo"),
                                           {sizeof(ayni_scope_v1), 0, nullptr},
                                           text("question")};
        return output.request_call(output.call_context, &request);
    };
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_FAILED, AYNI_CALL_EFFECT_NONE, {});
    pump(12);
    ASSERT_EQ(pending.size(), 2U);
    post(1, AYNI_CALL_FAILED, AYNI_CALL_EFFECT_NONE, {});
    pump(12);
    const auto task = work.task(work.goal(goal).last_task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(task.hold_reason, AYNI_REASON_STEP_CAPACITY);
    EXPECT_EQ(task.model_calls_used, 2U);
    EXPECT_EQ(pending.size(), 2U);
}

// C-COMMIT-007: storage acknowledgements, not merely applied state, unlock I/O.
TEST_F(ManagedCall, dispatch_waits_for_its_persisted_barrier) {
    const auto goal = create();
    uint64_t call = 0;
    for (unsigned round = 0; round < 80; ++round) {
        work.node().sim().complete_storage();
        work.node().sim().advance_us(10000);
        work.node().step(1);
        const auto task = work.goal(goal).last_task_id;
        if (task != 0U) {
            call = work.task(task).logical_call_id;
        }
        if (call != 0U && call_info(call).phase == AYNI_CALL_DISPATCHING) {
            break;
        }
    }
    ASSERT_NE(call, 0U);
    ASSERT_EQ(call_info(call).phase, AYNI_CALL_DISPATCHING);
    EXPECT_TRUE(pending.empty());
    for (unsigned round = 0; round < 8; ++round) {
        work.node().step();
    }
    EXPECT_TRUE(pending.empty());
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("done"));
    pump(12);
}

TEST_F(ManagedCall, revocation_after_preparation_prevents_provider_submission) {
    const auto goal = create();
    uint64_t call = 0;
    for (unsigned round = 0; round < 80; ++round) {
        work.node().sim().complete_storage();
        work.node().sim().advance_us(10000);
        work.node().step(1);
        const auto task = work.goal(goal).last_task_id;
        if (task != 0U) {
            call = work.task(task).logical_call_id;
        }
        if (call != 0U && call_info(call).phase == AYNI_CALL_DISPATCHING) {
            break;
        }
    }
    ASSERT_NE(call, 0U);
    ASSERT_EQ(call_info(call).phase, AYNI_CALL_DISPATCHING);
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, grants[0]};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &revoke, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    pump(16);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(call_info(call).outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(call_info(call).effect, AYNI_CALL_EFFECT_NONE);
}

// C-CALL-012: progress coalesces; reliable terminal is accepted exactly once.
TEST_F(ManagedCall, progress_is_monotonic_and_duplicate_terminal_is_rejected) {
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto &call = pending[0];
    ayni_provider_result_v1 progress{sizeof(progress),
                                     AYNI_CALL_PROGRESS,
                                     call.attempt,
                                     2,
                                     AYNI_OK,
                                     AYNI_REASON_NONE,
                                     AYNI_CALL_EFFECT_UNKNOWN,
                                     0,
                                     text("working"),
                                     0,
                                     0,
                                     nullptr};
    ASSERT_EQ(ayni_provider_post(call.completion, &progress), AYNI_OK);
    progress.sequence = 1;
    ASSERT_EQ(ayni_provider_post(call.completion, &progress), AYNI_OK);
    pump(4);
    EXPECT_EQ(call_info(call.call).progress_sequence, 2U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("done"));
    const ayni_provider_result_v1 duplicate{sizeof(duplicate),
                                            AYNI_CALL_COMPLETED,
                                            call.attempt,
                                            0,
                                            AYNI_OK,
                                            AYNI_REASON_NONE,
                                            AYNI_CALL_EFFECT_COMPLETE,
                                            0,
                                            text("replaced"),
                                            0,
                                            0,
                                            nullptr};
    EXPECT_EQ(ayni_provider_post(call.completion, &duplicate), AYNI_INVALID_STATE);
    pump(12);
    EXPECT_EQ(seen_result, "done");
}

class OneCallSlot : public ManagedCall {
    void SetUp() override {
        call_limit = 1;
        ManagedCall::SetUp();
    }
};
TEST_F(OneCallSlot, full_call_table_refuses_without_partial_task_count_or_resource_claim) {
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto second = create(agent_b);
    pump(12);
    const auto task = work.task(work.goal(second).last_task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(task.model_calls_used, 0U);
    EXPECT_EQ(task.logical_call_id, 0U);
    EXPECT_EQ(pending.size(), 1U);
}

class ModelCallLimit : public ManagedCall {
    void SetUp() override {
        call_limit = 16;
        step_limit = 16;
        ManagedCall::SetUp();
    }
};
// C-RUN-002/003: actual accepted model requests consume the persistent ceiling;
// raising the ceiling leaves used calls intact and permits only the added work.
TEST_F(ModelCallLimit, eight_model_calls_pause_until_owner_raises_the_same_task_limit) {
    unsigned requests = 0;
    work.strategy().answer = [&](const auto &, auto &output) {
        const auto key = std::string("model.") + std::to_string(++requests);
        const ayni_call_request_v1 request{sizeof(request),
                                           AYNI_POLICY_REQUEST_MODEL,
                                           1,
                                           0,
                                           {key.data(), static_cast<uint32_t>(key.size())},
                                           text("mock.echo"),
                                           {sizeof(ayni_scope_v1), 0, nullptr},
                                           text("question")};
        return output.request_call(output.call_context, &request);
    };
    const auto goal = create();
    pump(12);
    for (std::size_t at = 0; at < 8; ++at) {
        ASSERT_EQ(pending.size(), at + 1);
        post(at, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("answer"));
        pump(12);
    }
    const auto task = work.task(work.goal(goal).last_task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(task.model_calls_used, 8U);
    EXPECT_EQ(pending.size(), 8U);
    const ayni_task_raise_model_limit_v1 raise{sizeof(raise), 9, task.task_id};
    auto intent = work.intent(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, &raise, task.revision);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(pending.size(), 9U);
    EXPECT_EQ(work.task(task.task_id).model_calls_used, 9U);
    work.strategy().answer = [](const auto &snapshot, auto &output) {
        return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, output);
    };
    post(8, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("answer"));
    pump(12);
    EXPECT_EQ(work.task(task.task_id).lifecycle, AYNI_TASK_COMPLETED);
}

// C-LIFE-010: a weak query cannot turn an uncertain original execution into a
// known one, even if it returns a positive but nonauthoritative cached record.
TEST_F(BestEffortCall, nonauthoritative_found_result_keeps_execution_unknown) {
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending[0].call;
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    ASSERT_EQ(query_call(call, ++command).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_FOUND, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE,
               text("cached"));
    pump(8);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_RESULT_UNKNOWN);
    EXPECT_EQ(call_info(call).query_status, AYNI_RESULT_UNKNOWN);
    EXPECT_TRUE(seen_result.empty());
}

class MixedProviderCall : public ManagedCall {
    void SetUp() override {
        mixed_offers = true;
        ManagedCall::SetUp();
    }
};
// C-BIND-001/C-CALL-013: a Provider's unrelated egress offer must not make
// the saved local operation appear incompatible during an explicit query.
TEST_F(MixedProviderCall,
       local_operation_queries_the_original_mixed_provider_without_egress_destination) {
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending[0].call;
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    ASSERT_EQ(query_call(call, ++command).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_FOUND, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE,
               text("local"));
    pump(8);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_FINISHED);
    EXPECT_EQ(seen_result, "local");
}

// C-CALL-008/010: known failure/cancellation may still have unknown effects.
// Owner reconciliation must release that retention without reopening the Task.
TEST_F(ManagedCall, owner_can_reconcile_unknown_effects_after_known_failure) {
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending[0].call;
    const auto task = work.goal(goal).last_task_id;
    post(0, AYNI_CALL_FAILED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(12);
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_FAILED);
    ASSERT_EQ(query_call(call, ++command).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_FOUND, AYNI_CALL_FAILED, AYNI_CALL_EFFECT_NONE, {});
    pump(12);
    EXPECT_EQ(call_info(call).effect, AYNI_CALL_EFFECT_NONE);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_FAILED);
    EXPECT_EQ(pending.size(), 1U);
}

TEST_F(ManagedCall, owner_can_reconcile_unknown_effects_after_provider_cancellation) {
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending[0].call;
    const auto task = work.goal(goal).last_task_id;
    ASSERT_EQ(work.cancel_task(task).status, AYNI_OK);
    pump(4);
    post(0, AYNI_CALL_CANCELLED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(12);
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLED);
    ASSERT_EQ(query_call(call, ++command).status, AYNI_OK);
    pump(12);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_FOUND, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE,
               text("confirmed"));
    pump(12);
    EXPECT_EQ(call_info(call).effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_CANCELLED);
}

// C-RES-004/ADR-0047: trusted class controls the existing queue, without preemption.
TEST_F(ManagedCall, foreground_and_reminders_run_before_ordinary_and_background_work) {
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_OFFLINE),
              AYNI_OK);
    const auto background =
        create_profile("background", agent_a, AYNI_BACKGROUND_PERIOD_DEFAULT_US);
    const auto ordinary = create(agent_b);
    const auto foreground = create_profile("foreground");
    const auto reminder = create_profile("reminder");
    pump(24);
    ASSERT_TRUE(pending.empty());
    const std::array<uint64_t, 4> order{{foreground, reminder, ordinary, background}};
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    for (size_t at = 0; at < order.size(); ++at) {
        pump(16);
        ASSERT_EQ(pending.size(), at + 1);
        EXPECT_EQ(call_info(pending[at].call).task_id, work.goal(order[at]).last_task_id);
        post(at, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("done"));
    }
    pump(16);
}

// C-RES-005: a reminder survives ordinary resource timeouts and dispatches when available.
TEST_F(ManagedCall, reminder_waits_beyond_thirty_seconds_without_owner_resume) {
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_OFFLINE),
              AYNI_OK);
    const auto goal = create_profile("reminder");
    pump(10);
    const auto task = work.goal(goal).last_task_id;
    const auto call = work.task(task).logical_call_id;
    ASSERT_NE(call, 0U);
    work.node().sim().advance_us(AYNI_RESOURCE_WAIT_DEFAULT_US * 2);
    pump(6);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_RUNNING);
    EXPECT_EQ(call_info(call).wait_reason, AYNI_CALL_WAIT_OFFLINE);
    EXPECT_TRUE(pending.empty());
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].call, call);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("presented"));
    pump(16);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}

// ADR-0047: skipped work has no Provider attempt; the periodic cursor still schedules its next
// Task.
TEST_F(ManagedCall, background_wait_skips_one_task_and_next_period_can_execute) {
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_OFFLINE),
              AYNI_OK);
    const auto goal = create_profile("background", agent_a, AYNI_BACKGROUND_PERIOD_DEFAULT_US);
    pump(10);
    const auto task = work.goal(goal).last_task_id;
    const auto call = work.task(task).logical_call_id;
    ASSERT_NE(call, 0U);
    const auto next_due = work.goal(goal).next_due_us;
    work.node().sim().advance_us(AYNI_RESOURCE_WAIT_DEFAULT_US);
    pump(8);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_EXPIRED);
    EXPECT_EQ(work.task(task).model_calls_used, 1U);
    const auto skipped = call_info(call);
    EXPECT_EQ(skipped.outcome, AYNI_CALL_SKIPPED);
    EXPECT_EQ(skipped.status, AYNI_EXPIRED);
    EXPECT_EQ(skipped.reason, AYNI_REASON_RESOURCE_WAIT);
    EXPECT_EQ(skipped.effect, AYNI_CALL_EFFECT_NONE);
    EXPECT_EQ(skipped.attempt.request_id, 0U);
    EXPECT_TRUE(pending.empty());
    ayni_resource_queue_info_v1 queue{};
    queue.struct_size = sizeof(queue);
    ASSERT_EQ(ayni_get_resource_queue(work.kernel(), &queue), AYNI_OK);
    EXPECT_EQ(queue.capacity, work.node().config().resource_request_capacity);
    EXPECT_EQ(queue.waiting, 0U);
    EXPECT_EQ(queue.held, 0U);
    EXPECT_GE(queue.longest_wait_us, AYNI_RESOURCE_WAIT_DEFAULT_US);
    EXPECT_EQ(work.goal(goal).next_due_us, next_due);
    EXPECT_EQ(work.goal(goal).occurrences_skipped, 0U);
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    work.node().sim().advance_us(AYNI_BACKGROUND_PERIOD_DEFAULT_US);
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_NE(work.goal(goal).last_task_id, task);
    EXPECT_NE(pending[0].call, call);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("done"));
    pump(16);
}

// ADR-0047: only Core can prove that a queued call was skipped before Provider delivery.
TEST_F(ManagedCall, provider_cannot_report_the_core_only_skipped_outcome) {
    create();
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    const auto &call = pending[0];
    const ayni_provider_result_v1 result{sizeof(result),
                                         AYNI_CALL_SKIPPED,
                                         call.attempt,
                                         0,
                                         AYNI_EXPIRED,
                                         AYNI_REASON_RESOURCE_WAIT,
                                         AYNI_CALL_EFFECT_NONE,
                                         0,
                                         {},
                                         0,
                                         0,
                                         nullptr};
    EXPECT_EQ(ayni_provider_post(call.completion, &result), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(call_info(call.call).outcome, 0U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("done"));
    pump(16);
}

// C-RES-009: diagnostics account for both a current wait and an acquired group.
TEST_F(ManagedCall, resource_queue_snapshot_keeps_peak_wait_after_admission) {
    create();
    pump(16);
    create(agent_b);
    pump(10);
    ASSERT_EQ(pending.size(), 1U);
    ayni_resource_queue_info_v1 queue{};
    queue.struct_size = sizeof(queue);
    ASSERT_EQ(ayni_get_resource_queue(work.kernel(), &queue), AYNI_OK);
    EXPECT_EQ(queue.waiting, 1U);
    EXPECT_EQ(queue.held, 1U);
    EXPECT_GT(queue.longest_wait_us, 0U);
    const auto peak = queue.longest_wait_us;
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("first"));
    pump(16);
    ASSERT_EQ(pending.size(), 2U);
    ASSERT_EQ(ayni_get_resource_queue(work.kernel(), &queue), AYNI_OK);
    EXPECT_EQ(queue.waiting, 0U);
    EXPECT_EQ(queue.held, 1U);
    EXPECT_GE(queue.longest_wait_us, peak);
    const auto saved = queue;
    queue.struct_size -= 1;
    EXPECT_EQ(ayni_get_resource_queue(work.kernel(), &queue), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(queue.longest_wait_us, saved.longest_wait_us);
    post(1, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("second"));
    pump(16);
}

// ADR-0047: callers cannot raise background frequency through Goal creation arguments.
TEST_F(ManagedCall, background_period_floor_is_trusted_and_visible_in_goal_diagnostics) {
    auto body = WorkNode::create_body(work.instance(agent_a), 0, "background", 1, {},
                                      AYNI_TRIGGER_INTERVAL);
    body.trigger_period_us = AYNI_BACKGROUND_PERIOD_DEFAULT_US - 1;
    const auto refused = work.create(body);
    EXPECT_EQ(refused.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_BACKGROUND_FREQUENCY);
    body.trigger_period_us = AYNI_BACKGROUND_PERIOD_DEFAULT_US;
    const auto created = work.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    const auto profile = work.goal(created.result.object.id).work_profile;
    EXPECT_EQ(profile.work_class, AYNI_WORK_BACKGROUND);
    EXPECT_EQ(profile.background_period_min_us, AYNI_BACKGROUND_PERIOD_DEFAULT_US);
}

class MemoryCall : public ManagedCall {
    void SetUp() override {
        work.node().config().memory_capacity = 3;
        work.node().config().memory_body_max_bytes = 64;
        work.node().config().memory_dependency_capacity = 3;
        ManagedCall::SetUp();
    }
};
// ADR-0060: a normal asynchronous model result waits for storage before the next memory decision.
TEST_F(MemoryCall, model_result_becomes_unconfirmed_memory_after_its_durable_prefix) {
    const auto ordinary = work.strategy().answer;
    unsigned proposals = 0;
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &out) {
        for (uint32_t i = 0; i < snapshot.step_count; ++i) {
            if (string_of(snapshot.steps[i].step_key) == "remember") {
                return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, out);
            }
        }
        ayni_task_call_result_v1 result{};
        result.struct_size = sizeof(result);
        const auto status = snapshot.call_result(snapshot.call_context, text("ask"), &result);
        if (status == AYNI_NOT_FOUND) {
            return ordinary(snapshot, out);
        }
        EXPECT_EQ(status, AYNI_OK);
        ++proposals;
        ayni_memory_proposal_v1 proposal{};
        proposal.struct_size = sizeof(proposal);
        proposal.source = AYNI_MEMORY_MODEL;
        proposal.source_call_id = result.logical_call_id;
        proposal.step_key = text("remember");
        proposal.body = result.text;
        EXPECT_EQ(snapshot.memory->propose(snapshot.memory->context, &proposal), AYNI_OK);
        out.intents[0] = policy_intent(AYNI_POLICY_WRITE_MEMORY, snapshot.revision, "remember");
        out.intent_count = 1;
        return AYNI_OK;
    };
    const auto goal = create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("inferred preference"));
    // Completion is accepted, but withholding the ordinary ACK must not rerun or hold policy.
    for (unsigned i = 0; i < 8; ++i) {
        work.node().step();
    }
    EXPECT_EQ(proposals, 0U);
    EXPECT_NE(work.task(work.goal(goal).last_task_id).lifecycle, AYNI_TASK_PAUSED);
    pump(20);
    EXPECT_EQ(proposals, 1U);
    EXPECT_EQ(work.task(work.goal(goal).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
    ayni_memory_search_v1 query{sizeof(query),    AYNI_MEMORY_PRIVATE, work.instance(agent_a), 0,
                                text("inferred"), {nullptr, 0}};
    std::array<ayni_memory_info_v1, 3> rows{};
    uint32_t count = 0;
    ASSERT_EQ(
        ayni_search_memory(work.kernel(), work.session(), &query, rows.data(), rows.size(), &count),
        AYNI_OK);
    ASSERT_EQ(count, 1U);
    EXPECT_EQ(rows[0].source, AYNI_MEMORY_MODEL);
    EXPECT_EQ(rows[0].source_call_id, pending[0].call);
    EXPECT_EQ(rows[0].confirmed, 0U);
}

// ADR-0060: the public result copy is a content consumer, just like a strategy read.
TEST_F(MemoryCall, source_deletion_immediately_blocks_public_call_result_copy) {
    const ayni_memory_save_v1 value{sizeof(value),
                                    AYNI_MEMORY_PRIVATE,
                                    0,
                                    work.instance(agent_a),
                                    AYNI_PROFILE_NONE,
                                    0,
                                    text("preference"),
                                    nullptr,
                                    0,
                                    0,
                                    nullptr};
    auto intent = work.intent(AYNI_INTENT_MEMORY_SAVE, &value);
    intent.command_id = 600;
    const auto saved = work.submit(intent);
    ASSERT_EQ(saved.status, AYNI_OK);
    pump(12);
    const auto ordinary = work.strategy().answer;
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &out) {
        if (snapshot.step_count == 0) {
            ayni_memory_info_v1 metadata{};
            metadata.struct_size = sizeof(metadata);
            char body[64]{};
            uint32_t bytes = 0;
            EXPECT_EQ(snapshot.memory->read(snapshot.memory->context, saved.result.object.id,
                                            AYNI_MEMORY_CONTEXT, &metadata, body, sizeof(body),
                                            &bytes),
                      AYNI_OK);
        }
        return ordinary(snapshot, out);
    };
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("private answer"));
    pump(12);
    char answer[64]{};
    uint32_t bytes = 0;
    ASSERT_EQ(ayni_copy_call_result(work.kernel(), pending[0].call, answer, sizeof(answer), &bytes),
              AYNI_OK);
    const ayni_memory_target_v1 target{sizeof(target), 0, saved.result.object.id};
    intent = work.intent(AYNI_INTENT_MEMORY_DELETE, &target, 1);
    intent.command_id = 601;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    EXPECT_EQ(ayni_copy_call_result(work.kernel(), pending[0].call, answer, sizeof(answer), &bytes),
              AYNI_EXPIRED);
    pump(600);
    EXPECT_EQ(call_info(pending[0].call).outcome, AYNI_CALL_COMPLETED);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    EXPECT_EQ(call_info(pending[0].call).outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(ayni_copy_call_result(work.kernel(), pending[0].call, answer, sizeof(answer), &bytes),
              AYNI_EXPIRED);
}
} // namespace

// ADR-0060: privacy discards plaintext after checking Provider evidence, not before it.
TEST_F(MemoryCall, invalid_late_text_remains_unknown_after_its_source_is_deleted) {
    const ayni_memory_save_v1 value{sizeof(value),
                                    AYNI_MEMORY_PRIVATE,
                                    0,
                                    work.instance(agent_a),
                                    AYNI_PROFILE_NONE,
                                    0,
                                    text("preference"),
                                    nullptr,
                                    0,
                                    0,
                                    nullptr};
    auto intent = work.intent(AYNI_INTENT_MEMORY_SAVE, &value);
    intent.command_id = 600;
    const auto saved = work.submit(intent);
    ASSERT_EQ(saved.status, AYNI_OK);
    pump(12);
    const auto ordinary = work.strategy().answer;
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &out) {
        if (snapshot.step_count == 0) {
            ayni_memory_info_v1 metadata{};
            metadata.struct_size = sizeof(metadata);
            char body[64]{};
            uint32_t bytes = 0;
            EXPECT_EQ(snapshot.memory->read(snapshot.memory->context, saved.result.object.id,
                                            AYNI_MEMORY_CONTEXT, &metadata, body, sizeof(body),
                                            &bytes),
                      AYNI_OK);
        }
        return ordinary(snapshot, out);
    };
    create();
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    const ayni_memory_target_v1 target{sizeof(target), 0, saved.result.object.id};
    intent = work.intent(AYNI_INTENT_MEMORY_DELETE, &target, 1);
    intent.command_id = 601;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    const char invalid[] = {static_cast<char>(0xff)};
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, {invalid, sizeof(invalid)});
    pump(12);
    ayni_call_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_call(work.kernel(), pending[0].call, &info), AYNI_OK);
    EXPECT_EQ(info.outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(info.reason, AYNI_REASON_PROVIDER_RESULT);
}
