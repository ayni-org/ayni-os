#include "workspace_fixture.h"
// Normal public ABI call scenarios, C-CALL-002/012, C-COMMIT-007 and ADR-0045.
// Sim storage timing is explicit; no fault injection or restart campaign.
#include "goal_reauthorization_request.h"
#include "journal_test_support.h"
#include "kernel/service/command/command_records.h"
#include "snapshot_test_support.h"
#include "text_business_fixture.h"
#include "work/call/call_records.h"
#include "work/task/task_records.h"
#include "work_node.h"

#include <ayni/calls.h>
#include <ayni/memory.h>
#include <ayni/operations.h>
#include <cstdio>
#include <functional>
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
                if (background_action && !result.outcome) {
                    const ayni_operation_ref_v1 reference{snapshot.task_id, result.logical_call_id,
                                                          result.operation_id};
                    ayni_operation_info_v1 info{};
                    info.struct_size = sizeof(info);
                    const auto status =
                        snapshot.operation(snapshot.call_context, &reference, &info);
                    if (status != AYNI_OK) {
                        return status;
                    }
                    char key[AYNI_CATALOG_ID_MAX_BYTES];
                    const auto size = std::snprintf(key, sizeof(key), "wait.%u", snapshot.revision);
                    if (size < 1 || static_cast<size_t>(size) >= sizeof(key)) {
                        return AYNI_CAPACITY_EXCEEDED;
                    }
                    ayni_operation_control_v1 control{};
                    control.struct_size = sizeof(control);
                    control.kind = AYNI_OPERATION_WAIT;
                    control.step_key = {key, static_cast<uint32_t>(size)};
                    control.reference = reference;
                    control.after = info.frontier;
                    control.timeout_ms = AYNI_OPERATION_WAIT_MS_MAX;
                    return output.control_operation(output.call_context, &control);
                }
                seen_result = string_of(result.text);
                seen_call = result.logical_call_id;
                return report((result.outcome == AYNI_CALL_COMPLETED ||
                               (background_action && result.outcome == AYNI_CALL_CANCELLED))
                                  ? AYNI_POLICY_REPORT_COMPLETION
                                  : AYNI_POLICY_REPORT_FAILURE,
                              snapshot, output);
            }
            if (read != AYNI_NOT_FOUND) {
                return read;
            }
            char body[] = "hello";
            TextValue input({body, 5});
            const ayni_call_request_v1 request{
                sizeof(request),
                background_action ? AYNI_POLICY_REQUEST_CALL : AYNI_POLICY_REQUEST_MODEL,
                1,
                0,
                text("ask"),
                text("mock.echo"),
                {},
                {},
                input.view(),
                background_action ? AYNI_CALL_MODE_MANAGED : AYNI_CALL_MODE_FOREGROUND,
                0};
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
                               background_action ? AYNI_CALL_ACTION : AYNI_CALL_UNARY,
                               background_action ? AYNI_CALL_TOOL : AYNI_CALL_MODEL,
                               0,
                               AYNI_CALL_IDEMPOTENT,
                               query_authority,
                               AYNI_CALL_CANCEL_REQUEST,
                               0,
                               30000,
                               AYNI_CALL_INPUT_VALUE,
                               AYNI_CALL_RESULT_TEXT,
                               88,
                               64,
                               1,
                               &units,
                               nullptr};
        TextBusiness business(64, 64);
        capability.business = &business.definition;
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
            ayni::ValueReader input;
            ayni_string_v1 body{};
            if (input.read(request->value) != AYNI_OK ||
                !ayni::ValueReader::text(input.root(), body)) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            self.pending.push_back({request->attempt, request->logical_call_id,
                                    request->operation_id, completion, string_of(body), false});
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
                                      {nullptr, 0},
                                      0,
                                      0,
                                      nullptr};
        ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
    }
    uint64_t create(uint64_t agent = agent_a, uint32_t pause_group = 0) {
        auto body =
            WorkNode::create_body(work.instance(agent), 0, "chat", 1, {}, AYNI_TRIGGER_ONCE);
        body.pause_group = pause_group;
        const auto created = work.create(body);
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
                                             nullptr,
                                             {},
                                             {},
                                             {}};
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
                                             nullptr,
                                             {},
                                             {},
                                             {}};
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
    uint64_t ready_task(uint64_t goal) {
        auto &node = work.node();
        for (unsigned attempt = 0; attempt < 80; ++attempt) {
            node.sim().complete_storage();
            node.sim().advance_us(10000);
            node.step(1);
            const auto task = work.goal(goal).last_task_id;
            if (task && work.task(task).lifecycle == AYNI_TASK_READY &&
                node.sim().pending_storage() == 0U) {
                return task;
            }
        }
        return 0;
    }
    // Read the restored owner, rather than accepting a writer that omitted the receipt.
    void expect_snapshot_receipt(const ModelSnapshot &snapshot, uint64_t command_id,
                                 const Submitted &accepted) {
        Node target;
        target.config() = snapshot.config;
        target.on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        ASSERT_EQ(restore_model(snapshot, target), ayni::durability::BatchCheck::ok);
        const auto &commands = ayni::kernel::KernelTestProbe::work(target.kernel()).commands;
        const auto *restored = commands.mapped(owner_id, command_id);
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(restored->command_id, command_id);
        EXPECT_EQ(restored->commit_seq, accepted.result.commit_seq);
        EXPECT_EQ(restored->object.kind, accepted.result.object.kind);
        EXPECT_EQ(restored->object.id, accepted.result.object.id);
        EXPECT_EQ(restored->object.revision, accepted.result.object.revision);
        const auto &originals = ayni::kernel::KernelTestProbe::work(work.kernel()).commands;
        const auto *original = originals.mapped(owner_id, command_id);
        ASSERT_NE(original, nullptr);
        EXPECT_EQ(restored->kind, original->kind);
        EXPECT_EQ(restored->effect, original->effect);
        const auto before = originals.canonical(*original);
        const auto after = commands.canonical(*restored);
        EXPECT_EQ(std::vector<uint8_t>(after.data, after.data + after.size),
                  std::vector<uint8_t>(before.data, before.data + before.size));
    }
    WorkNode work;
    std::vector<Pending> queries;
    std::vector<uint64_t> grants;
    uint32_t step_limit = 8;
    uint32_t call_limit = 4;
    bool background_action = false;
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

// C-OP-001/003: native policies can wait/read while model I/O obeys the same-Agent FIFO.
class BackgroundAction : public ManagedCall {
  protected:
    void SetUp() override {
        background_action = true;
        ManagedCall::SetUp();
    }
};
TEST_F(BackgroundAction, resource_resume_readmits_the_same_call_without_a_foreground_link) {
    const auto goal = expire_resource_wait();
    const auto task = work.goal(goal).last_task_id;
    ASSERT_EQ(work.task(task).hold_reason, AYNI_REASON_RESOURCE_WAIT);
    EXPECT_EQ(work.task(task).logical_call_id, 0U);
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    ASSERT_EQ(resume_wait(task, ++command, work.task(task).revision).status, AYNI_OK);
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("done"));
    pump(12);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.task(task).model_calls_used, 0U);
}
TEST_F(BackgroundAction, pause_cancels_the_queued_call_and_clears_only_its_resource_overlay) {
    const auto goal = expire_resource_wait();
    const auto task = work.goal(goal).last_task_id;
    ASSERT_EQ(work.task(task).hold_reason, AYNI_REASON_RESOURCE_WAIT);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    pump(12);
    EXPECT_EQ(work.task(task).hold_status, AYNI_OK);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal).status, AYNI_OK);
    pump(12);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_TRUE(pending.empty());
}

struct MalformedTerminal {
    const char *name;
    ayni_call_outcome_t outcome;
    ayni_status_t status;
    ayni_reason_t reason;
    ayni_call_effect_t effect;
    uint32_t query_result = 0;
    const char *body = "";
};
class MalformedProviderResult : public ManagedCall,
                                public ::testing::WithParamInterface<MalformedTerminal> {};

// C-CALL-008/012: transport acceptance returns the borrow. Inconsistent
// terminal evidence must still become diagnostic UNKNOWN, never fabricated success.
TEST_P(MalformedProviderResult, returned_borrow_with_inconsistent_evidence_is_unknown) {
    create();
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    auto &delivery = pending.front();
    const auto &input = GetParam();
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = delivery.attempt;
    result.outcome = input.outcome;
    result.status = input.status;
    result.reason = input.reason;
    result.effect = input.effect;
    result.query_result = input.query_result;
    result.result = text(input.body);
    const auto posted = ayni_provider_post(delivery.completion, &result);
    delivery.ended = posted == AYNI_OK;
    ASSERT_EQ(posted, AYNI_OK);
    pump(12);
    const auto info = call_info(delivery.call);
    EXPECT_EQ(info.outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(info.status, AYNI_RESULT_UNKNOWN);
    EXPECT_EQ(info.reason, AYNI_REASON_PROVIDER_RESULT);
    EXPECT_EQ(info.effect, AYNI_CALL_EFFECT_UNKNOWN);
    EXPECT_EQ(info.result_bytes, 0U);
    EXPECT_TRUE(seen_result.empty());
}

INSTANTIATE_TEST_SUITE_P(
    TerminalEvidence, MalformedProviderResult,
    ::testing::Values(MalformedTerminal{"completed_status", AYNI_CALL_COMPLETED, AYNI_NOT_READY,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_COMPLETE},
                      MalformedTerminal{"completed_reason", AYNI_CALL_COMPLETED, AYNI_OK,
                                        AYNI_REASON_PROVIDER_RESULT, AYNI_CALL_EFFECT_COMPLETE},
                      MalformedTerminal{"completed_effect", AYNI_CALL_COMPLETED, AYNI_OK,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_PARTIAL},
                      MalformedTerminal{"failed_success", AYNI_CALL_FAILED, AYNI_OK,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE},
                      MalformedTerminal{"failed_unknown", AYNI_CALL_FAILED, AYNI_RESULT_UNKNOWN,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_UNKNOWN},
                      MalformedTerminal{"cancelled_status", AYNI_CALL_CANCELLED, AYNI_NOT_READY,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE},
                      MalformedTerminal{"cancelled_reason", AYNI_CALL_CANCELLED, AYNI_OK,
                                        AYNI_REASON_PROVIDER_RESULT, AYNI_CALL_EFFECT_NONE},
                      MalformedTerminal{"abandoned_status", AYNI_CALL_ABANDONED, AYNI_OK,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_UNKNOWN},
                      MalformedTerminal{"abandoned_effect", AYNI_CALL_ABANDONED,
                                        AYNI_RESULT_UNKNOWN, AYNI_REASON_NONE,
                                        AYNI_CALL_EFFECT_NONE},
                      MalformedTerminal{"abandoned_body", AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_UNKNOWN, 0, "unproven"},
                      MalformedTerminal{"unrequested_query", AYNI_CALL_COMPLETED, AYNI_OK,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_COMPLETE,
                                        AYNI_QUERY_RESULT_FOUND},
                      MalformedTerminal{"invalid_utf8", AYNI_CALL_COMPLETED, AYNI_OK,
                                        AYNI_REASON_NONE, AYNI_CALL_EFFECT_COMPLETE, 0, "\xff"}),
    [](const ::testing::TestParamInfo<MalformedTerminal> &scenario) {
        return scenario.param.name;
    });

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

// C-CALL-001/002 / C-POLICY-001: failed callback staging consumes no proposal,
// model charge, Call identity or resource; a later valid request still publishes once.
TEST_F(ManagedCall, malformed_staged_requests_leave_the_real_decision_available) {
    const auto ordinary = work.strategy().answer;
    uint32_t decisions = 0;
    work.strategy().answer = [&](const auto &, auto &output) {
        ++decisions;
        TextValue input(text("hello"));
        const ayni_call_request_v1 valid{sizeof(valid), AYNI_POLICY_REQUEST_MODEL, 1,  0,
                                         text("ask"),   text("mock.echo"),         {}, {},
                                         input.view(),  AYNI_CALL_MODE_FOREGROUND, 0};
        EXPECT_EQ(output.request_call(nullptr, &valid), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(output.request_call(output.call_context, nullptr), AYNI_INVALID_ARGUMENT);
        struct Invalid {
            void (*change)(ayni_call_request_v1 &);
            ayni_status_t status = AYNI_INVALID_ARGUMENT;
        };
        static const std::array<char, 65> oversized{};
        const Invalid invalid[] = {
            {[](auto &v) { v.struct_size = 0; }},
            {[](auto &v) { ++v.struct_size; }, AYNI_UNSUPPORTED_VERSION},
            {[](auto &v) { v.reserved = 1; }},
            {[](auto &v) { v.kind = UINT32_MAX; }},
            {[](auto &v) { v.step_key = {}; }},
            {[](auto &v) { v.binding_id = {}; }},
            {[](auto &v) { v.binding_id = text("unregistered"); }},
            {[](auto &v) { v.binding_version = 0; }},
            {[](auto &v) { ++v.binding_version; }},
            {[](auto &v) { v.kind = AYNI_POLICY_REQUEST_CALL; }},
            {[](auto &v) { v.value = {}; }},
            {[](auto &v) { v.arguments.field_count = 1; }},
            {[](auto &v) { v.text = {nullptr, 1}; }},
            {[](auto &v) { v.text = {"\xff", 1}; }},
            {[](auto &v) { v.text = {oversized.data(), oversized.size()}; }},
        };
        for (const auto &fault : invalid) {
            auto candidate = valid;
            fault.change(candidate);
            EXPECT_EQ(output.request_call(output.call_context, &candidate), fault.status);
            EXPECT_EQ(output.intent_count, 0U);
        }
        for (const auto kind : {AYNI_POLICY_CLOSE_SUBSCRIPTION, AYNI_POLICY_WAIT_SUBSCRIPTION}) {
            ayni_call_request_v1 target{};
            target.struct_size = sizeof(target);
            target.kind = kind;
            target.step_key = text("ask");
            const auto unexpected_routes = {
                +[](ayni_call_request_v1 &v) { v.binding_version = 1; },
                +[](ayni_call_request_v1 &v) { v.binding_id = text("mock.echo"); },
                +[](ayni_call_request_v1 &v) { v.text = text("extra"); },
                +[](ayni_call_request_v1 &v) { v.arguments.struct_size = sizeof(ayni_scope_v1); },
                +[](ayni_call_request_v1 &v) { v.arguments.field_count = 1; },
            };
            for (const auto change : unexpected_routes) {
                auto candidate = target;
                change(candidate);
                EXPECT_EQ(output.request_call(output.call_context, &candidate),
                          AYNI_INVALID_ARGUMENT);
                EXPECT_EQ(output.intent_count, 0U);
            }
            EXPECT_EQ(output.request_call(output.call_context, &target), AYNI_NOT_FOUND);
        }
        EXPECT_EQ(output.request_call(output.call_context, &valid), AYNI_OK);
        EXPECT_EQ(output.request_call(output.call_context, &valid), AYNI_CAPACITY_EXCEEDED);
        EXPECT_EQ(output.intent_count, 1U);
        return AYNI_OK;
    };
    const auto goal = create();
    pump(16);
    ASSERT_EQ(decisions, 1U);
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending.front().text, "hello");
    const auto task = work.goal(goal).last_task_id;
    EXPECT_EQ(work.task(task).model_calls_used, 1U);
    EXPECT_EQ(ayni::kernel::KernelTestProbe::work(work.kernel()).calls.size(), 1U);
    work.strategy().answer = ordinary;
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("answer"));
    pump(12);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(seen_result, "answer");
}

// C-CALL-002 / C-SUB-001: Task-scoped views expose no result when no step
// exists, and malformed arguments cannot publish even partial output metadata.
TEST_F(ManagedCall, callback_result_views_reject_bad_arguments_and_preserve_missing_outputs) {
    uint32_t decisions = 0;
    work.strategy().answer = [&](const auto &snapshot, auto &output) {
        ++decisions;
        const auto check = [&](auto read, auto &out, ayni_status_t future_shape) {
            EXPECT_EQ(read(nullptr, &out), AYNI_INVALID_ARGUMENT);
            EXPECT_EQ(read(snapshot.call_context, static_cast<decltype(&out)>(nullptr)),
                      AYNI_INVALID_ARGUMENT);
            out.struct_size = 0;
            EXPECT_EQ(read(snapshot.call_context, &out), AYNI_INVALID_ARGUMENT);
            out.struct_size = sizeof(out) + 1;
            EXPECT_EQ(read(snapshot.call_context, &out), future_shape);
            out.struct_size = sizeof(out);
            EXPECT_EQ(read(snapshot.call_context, &out), AYNI_NOT_FOUND);
        };
        ayni_task_call_result_v1 result{};
        result.struct_size = sizeof(result);
        result.outcome = 99;
        check(
            [&](const void *context, ayni_task_call_result_v1 *out) {
                return snapshot.call_result(context, text("missing"), out);
            },
            result, AYNI_UNSUPPORTED_VERSION);
        EXPECT_EQ(result.outcome, 99U);
        ayni_subscription_info_v1 subscription{};
        subscription.struct_size = sizeof(subscription);
        subscription.phase = 99;
        check(
            [&](const void *context, ayni_subscription_info_v1 *out) {
                return snapshot.subscription(context, text("missing"), out);
            },
            subscription, AYNI_UNSUPPORTED_VERSION);
        EXPECT_EQ(subscription.phase, 99U);
        // C-VER-004 / C-OP-002: new operation views/controls share the standard ABI shape rule.
        const ayni_operation_ref_v1 reference{snapshot.task_id, snapshot.task_id, snapshot.task_id};
        ayni_operation_info_v1 operation{};
        operation.struct_size = sizeof(operation);
        operation.form = 99;
        check(
            [&](const void *context, ayni_operation_info_v1 *out) {
                return snapshot.operation(context, &reference, out);
            },
            operation, AYNI_UNSUPPORTED_VERSION);
        EXPECT_EQ(operation.form, 99U);
        ayni_operation_control_v1 control{};
        EXPECT_EQ(output.control_operation(output.call_context, &control), AYNI_INVALID_ARGUMENT);
        control.struct_size = sizeof(control) + 1;
        EXPECT_EQ(output.control_operation(output.call_context, &control),
                  AYNI_UNSUPPORTED_VERSION);
        ayni_observation_info_v1 observation{};
        observation.struct_size = sizeof(observation);
        observation.usable = 99;
        check(
            [&](const void *context, ayni_observation_info_v1 *out) {
                return snapshot.subscription_observation(context, text("missing"), out);
            },
            observation, AYNI_UNSUPPORTED_VERSION);
        EXPECT_EQ(observation.usable, 99U);
        return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, output);
    };
    const auto goal = create();
    pump(12);
    ASSERT_EQ(decisions, 1U);
    EXPECT_EQ(work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(ayni::kernel::KernelTestProbe::work(work.kernel()).calls.size(), 0U);
}

// ADR-0065: a brief group pause remains a cancellation request for an in-flight model.
TEST_F(ManagedCall, group_pause_then_resume_before_step_cancels_the_original_delivery) {
    create(agent_a, 1);
    pump(12);
    ASSERT_EQ(pending.size(), 1U);
    cancel_posts_terminal = true;
    ayni_pause_group_set_v1 body{sizeof(body), 1, 1, 0};
    auto intent = work.intent(AYNI_INTENT_PAUSE_GROUP_SET, &body, 0);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    body.paused = 0;
    intent.expected_revision = 1;
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    pump(12);
    EXPECT_EQ(cancellations, 1U);
    EXPECT_EQ(pending.size(), 1U);
    EXPECT_TRUE(pending[0].ended);
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

// C-STORE-007 / ADR-0045: an exact resume command is valid both at the
// resumed state and after its same Call has advanced to terminal completion.
TEST_F(ManagedCall, snapshot_retains_wait_resume_before_and_after_call_completion) {
    const auto goal = expire_resource_wait();
    const auto task = work.goal(goal).last_task_id;
    const auto resumed = resume_wait(task, ++command, work.task(task).revision);
    ASSERT_EQ(resumed.status, AYNI_OK);
    const auto original = snapshot_model(work.node());
    expect_snapshot_receipt(original, command, resumed);
    const auto &tasks = ayni::kernel::KernelTestProbe::work(work.kernel()).tasks;
    uint32_t task_index = 0;
    ASSERT_TRUE(tasks.find(task, task_index));
    const auto changes = {
        +[](ayni::work::Task &v) { ++v.revision; },
        +[](ayni::work::Task &v) { v.hold_status = AYNI_BUSY; },
        +[](ayni::work::Task &v) { v.hold_reason = AYNI_REASON_RESOURCE_WAIT; },
    };
    for (const auto change : changes) {
        auto snapshot = original;
        auto changed = tasks.at(task_index);
        change(changed);
        uint32_t selected = 0;
        for (auto &row : snapshot.records) {
            if (row.header.type != ayni::work::task_state_record_type) {
                continue;
            }
            ayni::work::TaskStateRecord state{};
            ASSERT_TRUE(
                ayni::work::read_task_state_record(row.body.data(), row.header.length, state));
            if (state.task_id == task) {
                ++selected;
                const auto before = row.body;
                ayni::work::write_task_state_record(changed, row.body.data(), state.change);
                ASSERT_NE(row.body, before);
            }
        }
        ASSERT_EQ(selected, 1U);
        Node target;
        target.config() = snapshot.config;
        target.on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        EXPECT_EQ(restore_model(snapshot, target), ayni::durability::BatchCheck::corrupt);
    }
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("done"));
    pump(12);
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    expect_snapshot_receipt(snapshot_model(work.node()), command, resumed);
}

// C-STORE-009 / ADR-0045: journal replay restores the accepted wait-resume
// with its original Call and charge, before or after that Call completes.
TEST_F(ManagedCall, journal_replays_wait_resume_identity_and_rejects_broken_atomic_partners) {
    const auto goal = expire_resource_wait();
    const auto task = work.goal(goal).last_task_id;
    const auto before = work.task(task);
    const auto call = before.logical_call_id;
    const auto operation = call_info(call).operation_id;
    const auto command_id = ++command;
    const auto accepted = resume_wait(task, command_id, before.revision);
    ASSERT_EQ(accepted.status, AYNI_OK);
    pump(8);
    ASSERT_GE(work.node().info().durable_seq, accepted.result.commit_seq);
    const auto original = work.node().sim().log();
    const auto check_recovery = [&](const auto &log, bool valid) {
        Node target;
        target.config() = work.node().config();
        target.on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        target.sim().log() = log;
        target.create_and_start();
        target.recover();
        if (!valid) {
            EXPECT_EQ(target.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
            EXPECT_EQ(target.info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
            return;
        }
        ASSERT_EQ(target.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        const auto &model = ayni::kernel::KernelTestProbe::work(target.kernel());
        uint32_t task_index = 0, call_index = 0;
        ASSERT_TRUE(model.tasks.find(task, task_index));
        ASSERT_TRUE(model.calls.find(call, call_index));
        EXPECT_EQ(model.calls.identity(call_index).operation_id, operation);
        EXPECT_EQ(model.tasks.at(task_index).model_calls.used, before.model_calls_used);
        const auto *receipt = model.commands.mapped(owner_id, command_id);
        ASSERT_NE(receipt, nullptr);
        EXPECT_EQ(receipt->kind, AYNI_INTENT_TASK_RESUME_WAIT);
        EXPECT_EQ(receipt->commit_seq, accepted.result.commit_seq);
        EXPECT_EQ(receipt->object.id, task);
        EXPECT_EQ(receipt->object.revision, accepted.result.object.revision);
    };
    check_recovery(original, true);
    std::size_t selected = original.size();
    JournalBatch batch{};
    for (std::size_t at = 0; at < original.size(); ++at) {
        if (original[at].commit_seq == accepted.result.commit_seq) {
            selected = at;
            batch = read_stored_batch(original[at]);
        }
    }
    ASSERT_LT(selected, original.size());
    constexpr std::size_t atomic_partners = 2; // Task state, then its exact owner command.
    ASSERT_EQ(batch.records.size(), atomic_partners);
    ASSERT_EQ(batch.records.front().header.type, ayni::work::task_state_record_type);
    ASSERT_EQ(batch.records.back().header.type, ayni::kernel::command_record_type);
    for (unsigned fault = 0; fault < 3; ++fault) {
        auto changed = batch;
        if (fault == 0) {
            changed.records.pop_back();
        } else if (fault == 1) {
            const auto repeated = changed.records.front();
            changed.records.insert(changed.records.begin(), repeated);
        } else {
            std::reverse(changed.records.begin(), changed.records.end());
        }
        auto log = original;
        log[selected] = write_stored_batch(changed);
        check_recovery(log, false);
    }
    // C-STORE-004 / ADR-0082: even optional unknown records at every atomic
    // partner position must reject before any writer/output and preserve source bytes.
    for (std::size_t position = 0; position <= batch.records.size(); ++position) {
        auto changed = batch;
        changed.records.insert(changed.records.begin() + static_cast<std::ptrdiff_t>(position),
                               JournalRecord{{UINT16_MAX, 1, 0, 0}, {}});
        auto log = original;
        log[selected] = write_stored_batch(changed);
        Node target;
        target.config() = work.node().config();
        target.on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
        target.sim().log() = log;
        target.create_and_start();
        target.recover();
        EXPECT_EQ(target.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_TRUE(target.info().restriction == AYNI_RESTRICTION_LOG_UNSUPPORTED ||
                    target.info().restriction == AYNI_RESTRICTION_LOG_CORRUPT);
        ASSERT_EQ(target.sim().log().size(), log.size());
        for (std::size_t index = 0; index < log.size(); ++index) {
            EXPECT_EQ(target.sim().log()[index].bytes, log[index].bytes);
        }
    }
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), text("model.slot"), AYNI_RESOURCE_ONLINE),
              AYNI_OK);
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    post(0, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE, text("done"));
    pump(12);
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    check_recovery(work.node().sim().log(), true);
}

// C-CALL-002/011 / C-STORE-009: a full-row Call update is not a new
// acceptance. Intrinsically valid facts must still match its original Task,
// route, action identity and exact enclosing batch before they can be restored.
TEST_F(ManagedCall, journal_call_updates_require_original_task_route_and_batch_identity) {
    const auto goal = create();
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending.front().call;
    const auto other_goal = create(agent_b);
    pump(8);
    const auto other_task = work.goal(other_goal).last_task_id;
    ASSERT_NE(other_task, 0U);
    ayni_provider_result_v1 progress{};
    progress.struct_size = sizeof(progress);
    progress.outcome = AYNI_CALL_PROGRESS;
    progress.attempt = pending.front().attempt;
    progress.sequence = 1;
    progress.status = AYNI_OK;
    progress.effect = AYNI_CALL_EFFECT_UNKNOWN;
    ASSERT_EQ(ayni_provider_post(pending.front().completion, &progress), AYNI_OK);
    pump(8);
    const auto current = call_info(call);
    ASSERT_EQ(current.progress_sequence, 1U);
    ASSERT_GE(work.node().info().durable_seq, current.requires_seq);
    const auto log = work.node().sim().log();
    const auto configure = [&](Node &target) {
        target.config() = work.node().config();
        target.on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
    };
    Node control;
    configure(control);
    control.sim().log() = log;
    control.create_and_start();
    control.recover();
    ASSERT_EQ(control.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    std::size_t selected = log.size();
    JournalBatch original{};
    for (std::size_t at = 0; at < log.size(); ++at) {
        if (log[at].commit_seq == current.requires_seq) {
            selected = at;
            original = read_stored_batch(log[at]);
        }
    }
    ASSERT_LT(selected, log.size());
    auto row =
        std::find_if(original.records.begin(), original.records.end(), [](const auto &entry) {
            return entry.header.type == ayni::work::call_record_type;
        });
    ASSERT_NE(row, original.records.end());
    const auto row_index = static_cast<std::size_t>(row - original.records.begin());
    // White-box: schema 10 deltas borrow immutable admission facts; they cannot encode a route
    // replacement.
    const auto &calls = ayni::kernel::KernelTestProbe::work(work.kernel()).calls;
    uint32_t call_index = 0;
    ASSERT_TRUE(calls.find(call, call_index));
    const auto admitted = calls.record(call_index);
    struct Variant {
        const char *name;
        std::function<void(ayni::work::CallRecord &)> change;
    };
    const Variant variants[] = {
        {"writer_boot",
         [](auto &r) {
             ++r.state.latest.boot_epoch;
             ++r.action->latest.boot_epoch;
         }},
        {"commit",
         [](auto &r) {
             ++r.state.latest.transition.commit_seq;
             ++r.action->latest.transition.commit_seq;
         }},
        {"transition",
         [](auto &r) {
             ++r.state.latest.transition.transition_id;
             ++r.action->latest.transition.transition_id;
         }},
        {"goal_is_not_task", [&](auto &r) { r.identity.task_id = goal; }},
        {"different_owner",
         [&](auto &r) { r.identity.owner_instance_id = work.instance(agent_b); }},
        {"different_task", [&](auto &r) { r.identity.task_id = other_task; }},
        {"other_tasks_call",
         [&](auto &r) {
             r.identity.task_id = other_task;
             r.identity.owner_instance_id = work.instance(agent_b);
         }},
        {"binding",
         [](auto &r) {
             ++r.identity.binding_version;
             ++r.action->identity.binding_version;
         }},
        {"terminal_without_task_partner",
         [](auto &r) {
             r.state.outcome = AYNI_CALL_COMPLETED;
             r.state.status = AYNI_OK;
             r.state.reason = AYNI_REASON_NONE;
             r.action->phase = ayni::work::ActionPhase::completed;
             r.action->effect = ayni::work::ActionEffect::complete;
             r.action->evidence = ayni::work::ActionEvidence::provider_result;
         }},
    };
    for (const auto &variant : variants) {
        SCOPED_TRACE(variant.name);
        ayni::work::CallRecord record{};
        ASSERT_TRUE(ayni::work::read_call_delta(::ayni::test::codec_workspace(), admitted,
                                                row->body.data(), row->header.length, record));
        ASSERT_EQ(record.identity.call_id, call);
        ASSERT_TRUE(record.action.has_value());
        variant.change(record);
        ASSERT_TRUE(ayni::work::CallTable::valid_record(::ayni::test::codec_workspace(), record));
        auto changed = original;
        const bool replacement =
            record.identity.task_id != admitted.identity.task_id ||
            record.identity.owner_instance_id != admitted.identity.owner_instance_id ||
            record.identity.binding_version != admitted.identity.binding_version;
        const auto size = replacement ? ayni::work::call_record_size(record)
                                      : ayni::work::call_delta_size(record);
        std::vector<uint8_t> bytes(size);
        if (replacement) {
            ayni::work::write_call_record(record, bytes.data());
        } else {
            ayni::work::write_call_delta(record, bytes.data());
        }
        changed.records[row_index].header.version =
            replacement ? ayni::work::call_record_version : ayni::work::call_delta_version;
        changed.records[row_index].header.length = size;
        changed.records[row_index].body = std::move(bytes);
        auto candidate = log;
        candidate[selected] = write_stored_batch(changed);
        Node recovered;
        configure(recovered);
        recovered.sim().log() = std::move(candidate);
        recovered.create_and_start();
        recovered.recover();
        EXPECT_EQ(recovered.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(recovered.info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
        EXPECT_EQ(ayni::kernel::KernelTestProbe::work(recovered.kernel()).calls.size(), 0U);
    }
    Node smaller;
    configure(smaller);
    smaller.config().call_capacity = 1;
    smaller.sim().log() = log;
    smaller.create_and_start();
    smaller.recover();
    EXPECT_EQ(smaller.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(smaller.info().restriction, AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY);
}

// C-STORE-007 / C-CALL-011: the retained query command proves its exact
// requested query, then remains a historical receipt after reconciliation.
TEST_F(ManagedCall, snapshot_retains_requested_and_resolved_query_receipts) {
    create();
    pump(16);
    ASSERT_EQ(pending.size(), 1U);
    const auto call = pending.front().call;
    post(0, AYNI_CALL_ABANDONED, AYNI_CALL_EFFECT_UNKNOWN, {});
    pump(8);
    const auto accepted = query_call(call, ++command);
    ASSERT_EQ(accepted.status, AYNI_OK);
    const auto original = snapshot_model(work.node());
    expect_snapshot_receipt(original, command, accepted);
    auto candidate = original;
    uint32_t selected = 0;
    for (auto &row : candidate.records) {
        if (row.header.type != ayni::work::call_record_type) {
            continue;
        }
        ayni::work::CallRecord record{};
        ASSERT_TRUE(ayni::work::read_call_record(::ayni::test::codec_workspace(), row.body.data(),
                                                 row.header.length, record));
        if (record.identity.call_id == call) {
            ++selected;
            ASSERT_TRUE(record.state.query.has_value());
            ++record.state.query->generation;
            ASSERT_TRUE(
                ayni::work::CallTable::valid_record(::ayni::test::codec_workspace(), record));
            std::vector<uint8_t> bytes(ayni::work::call_record_size(record));
            ayni::work::write_call_record(record, bytes.data());
            row.body = std::move(bytes);
        }
    }
    ASSERT_EQ(selected, 1U);
    Node target;
    target.config() = candidate.config;
    target.on_create([this](ayni_kernel_t *kernel) { register_profile(kernel); });
    EXPECT_EQ(restore_model(candidate, target), ayni::durability::BatchCheck::corrupt);
    pump(16);
    ASSERT_EQ(queries.size(), 1U);
    post_query(0, AYNI_QUERY_RESULT_FOUND, AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE,
               text("found"));
    pump(12);
    ASSERT_EQ(call_info(call).outcome, AYNI_CALL_COMPLETED);
    expect_snapshot_receipt(snapshot_model(work.node()), command, accepted);
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
        TextValue input(text("question"));
        const ayni_call_request_v1 request{sizeof(request),
                                           AYNI_POLICY_REQUEST_MODEL,
                                           1,
                                           0,
                                           {key.data(), static_cast<uint32_t>(key.size())},
                                           text("mock.echo"),
                                           {},
                                           {},
                                           input.view(),
                                           AYNI_CALL_MODE_FOREGROUND,
                                           0};
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

// The group port holds one append at a time; completion publishes the whole prefix.
class CallAppendGroup {
  public:
    explicit CallAppendGroup(Node &node) : node_(node) {
        active_ = this;
        ayni::kernel::KernelTestProbe::host(node_.kernel()).storage_append_many = hold;
    }
    ~CallAppendGroup() {
        complete();
        ayni::kernel::KernelTestProbe::host(node_.kernel()).storage_append_many = nullptr;
        active_ = nullptr;
    }
    void complete() {
        if (completion_) {
            auto &log = node_.sim().log();
            log.insert(log.end(), batches.begin(), batches.end());
            const ayni_completion_result_v1 result{sizeof(result), AYNI_WRITE_OK, attempt_, 0, 0};
            EXPECT_EQ(ayni_completion_post(completion_, &result), AYNI_OK);
            completion_ = nullptr;
        }
    }
    std::vector<ayni::hosts::sim::StoredBatch> batches;

  private:
    static ayni_submit_result_t hold(void *, const ayni_storage_batch_v1 *items, uint32_t count,
                                     ayni_attempt_ref_v1 attempt, ayni_completion_t *completion) {
        auto &self = *active_;
        if (self.completion_) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.batches.clear();
        for (uint32_t index = 0; index < count; ++index) {
            const auto &item = items[index];
            self.batches.push_back({item.commit_seq, bytes_of(item.encoded), false});
        }
        self.attempt_ = attempt;
        self.completion_ = completion;
        return AYNI_SUBMIT_ACCEPTED;
    }
    Node &node_;
    ayni_attempt_ref_v1 attempt_{};
    ayni_completion_t *completion_ = nullptr;
    static inline thread_local CallAppendGroup *active_ = nullptr;
};

// C-COMMIT-006/007: three distinct facts can join one physical append.
TEST_F(ManagedCall, fresh_call_journals_accept_prepare_and_barrier_in_one_group) {
    const auto task = ready_task(create());
    ASSERT_NE(task, 0U);
    auto &node = work.node();
    CallAppendGroup append(node);
    const auto report = node.step();
    ASSERT_EQ(report.items_done, 3U);
    const auto call = work.task(task).logical_call_id;
    ASSERT_NE(call, 0U);
    ASSERT_EQ(call_info(call).phase, AYNI_CALL_DISPATCHING);
    ASSERT_EQ(append.batches.size(), 3U);
    ayni::work::CallRecord decoded{};
    const std::array<ayni::work::ActionPhase, 2> phases{ayni::work::ActionPhase::prepared,
                                                        ayni::work::ActionPhase::dispatching};
    std::array<JournalBatch, 3> records;
    for (size_t index = 0; index < append.batches.size(); ++index) {
        // Decoded Call rows borrow their encoded domain and input through the last delta.
        auto &batch = records[index];
        batch = read_stored_batch(append.batches[index]);
        EXPECT_EQ(batch.identity.commit_seq, report.durable_seq + 1U + index);
        const auto row =
            std::find_if(batch.records.begin(), batch.records.end(), [](const auto &entry) {
                return entry.header.type == ayni::work::call_record_type;
            });
        ASSERT_NE(row, batch.records.end());
        ayni::work::CallRecord next{};
        if (index == 0U) {
            ASSERT_TRUE(ayni::work::read_call_record(codec_workspace(), row->body.data(),
                                                     row->header.length, next));
            EXPECT_FALSE(next.action);
        } else {
            ASSERT_TRUE(ayni::work::read_call_delta(codec_workspace(), decoded, row->body.data(),
                                                    row->header.length, next));
            ASSERT_TRUE(next.action.has_value());
            EXPECT_EQ(next.action->phase, phases[index - 1U]);
        }
        EXPECT_EQ(next.identity.call_id, call);
        decoded = next;
    }
    EXPECT_EQ(call_info(call).requires_seq, append.batches.back().commit_seq);
    node.step();
    EXPECT_TRUE(pending.empty());
    append.complete();
    node.step();
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending.front().call, call);
}

// C-COMMIT-006: the optional preparations yield to step bounds and single-append hosts.
TEST_F(ManagedCall, preparation_yields_to_one_item_step_budget) {
    const auto task = ready_task(create());
    ASSERT_NE(task, 0U);
    auto &node = work.node();
    CallAppendGroup append(node);
    EXPECT_EQ(node.step(1).items_done, 1U);
    const auto call = work.task(task).logical_call_id;
    ASSERT_NE(call, 0U);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_QUEUED);
    ASSERT_EQ(append.batches.size(), 1U);
    EXPECT_TRUE(pending.empty());
}

TEST_F(ManagedCall, single_append_host_keeps_acceptance_as_the_only_new_fact) {
    const auto task = ready_task(create());
    ASSERT_NE(task, 0U);
    auto &node = work.node();
    EXPECT_EQ(node.step().items_done, 1U);
    const auto call = work.task(task).logical_call_id;
    ASSERT_NE(call, 0U);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_QUEUED);
    ASSERT_TRUE(node.sim().pending_append());
    EXPECT_EQ(node.sim().pending_append()->commit_seq, call_info(call).requires_seq);
    EXPECT_TRUE(pending.empty());
}

// C-RES-002/004: a queued older Call gets the available claim before a fresh decision.
TEST_F(ManagedCall, older_resource_waiter_prepares_before_newly_admitted_call) {
    auto &node = work.node();
    const auto slot = text("model.slot");
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), slot, AYNI_RESOURCE_OFFLINE), AYNI_OK);
    const auto older_task = ready_task(create(agent_b));
    ASSERT_NE(older_task, 0U);
    node.step(1);
    const auto older = work.task(older_task).logical_call_id;
    ASSERT_NE(older, 0U);
    ASSERT_EQ(call_info(older).phase, AYNI_CALL_QUEUED);
    const auto fresh_task = ready_task(create(agent_a));
    ASSERT_NE(fresh_task, 0U);
    ASSERT_EQ(ayni_set_resource_health(work.kernel(), slot, AYNI_RESOURCE_ONLINE), AYNI_OK);

    CallAppendGroup append(node);
    node.step();
    const auto fresh = work.task(fresh_task).logical_call_id;
    ASSERT_NE(fresh, 0U);
    EXPECT_EQ(call_info(older).phase, AYNI_CALL_DISPATCHING);
    EXPECT_EQ(call_info(fresh).phase, AYNI_CALL_QUEUED);
    EXPECT_LT(call_info(older).requires_seq, call_info(fresh).requires_seq);
    EXPECT_TRUE(pending.empty());
}

class QueuedReauthorization : public ManagedCall, public ::testing::WithParamInterface<bool> {};

// C-COMMIT-007: queued progress waits for the binding; a stop remains visible while waiting.
TEST_P(QueuedReauthorization, queued_call_wakes_for_durability_or_stop) {
    auto &node = work.node();
    const ayni_grant_create_v1 replacement{sizeof(replacement),
                                           1,
                                           text("echo"),
                                           work.instance(agent_a),
                                           0,
                                           AYNI_GRANT_PERMANENT,
                                           0,
                                           0,
                                           {sizeof(ayni_scope_v1), 0, nullptr}};
    auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &replacement);
    intent.command_id = ++command;
    const auto grant = work.submit(intent);
    ASSERT_EQ(grant.status, AYNI_OK);
    pump(4);

    const auto goal = create();
    const auto task = ready_task(goal);
    ASSERT_NE(task, 0U);
    node.step(1); // Admit the Call but leave its resource request queued.
    const auto call = work.task(task).logical_call_id;
    ASSERT_NE(call, 0U);
    ASSERT_EQ(call_info(call).phase, AYNI_CALL_QUEUED);
    ASSERT_EQ(node.sim().complete_storage(), 1U);
    node.step(1);
    const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, grants[0]};
    intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &revoke, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    node.sim().advance_us(10000);
    node.step(1); // Apply revocation without running ordinary calls or goal cleanup.
    ASSERT_EQ(node.sim().complete_storage(), 1U);
    node.step(1); // Consume only the storage ACK.

    ReauthorizationRequest request(work.kernel(), goal);
    intent = work.intent(AYNI_INTENT_GOAL_REAUTHORIZE, &request.body, request.echo.info.revision);
    intent.command_id = ++command;
    const auto rebound = work.submit(intent);
    ASSERT_EQ(rebound.status, AYNI_OK);
    node.sim().advance_us(10000);
    auto report = node.step();
    EXPECT_LT(report.durable_seq, rebound.result.commit_seq);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_QUEUED);
    EXPECT_TRUE(pending.empty());
    ASSERT_EQ(node.sim().pending_storage(), 1U);
    if (GetParam()) {
        const ayni_task_target_v1 target{sizeof(target), 0, task};
        intent = work.intent(AYNI_INTENT_TASK_CANCEL, &target, work.task(task).revision);
        intent.command_id = ++command;
        ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        EXPECT_EQ(node.step(1).items_done, 1U);
        EXPECT_EQ(call_info(call).outcome, AYNI_CALL_CANCELLED);
        EXPECT_EQ(call_info(call).effect, AYNI_CALL_EFFECT_NONE);
        EXPECT_TRUE(pending.empty());
        return;
    }
    EXPECT_EQ(node.sim().complete_storage(), 1U);
    pump(8);
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending.front().call, call);
}

INSTANTIATE_TEST_SUITE_P(ResumeOrStop, QueuedReauthorization, ::testing::Bool());

TEST_F(ManagedCall, preparation_yields_when_policy_spends_the_time_budget) {
    const auto task = ready_task(create());
    ASSERT_NE(task, 0U);
    auto &node = work.node();
    const auto answer = work.strategy().answer;
    work.strategy().answer = [&](const auto &snapshot, auto &output) {
        node.sim().advance_us(100);
        return answer(snapshot, output);
    };
    CallAppendGroup append(node);
    EXPECT_EQ(node.step(10, 100).items_done, 1U);
    work.strategy().answer = answer;
    const auto call = work.task(task).logical_call_id;
    ASSERT_NE(call, 0U);
    EXPECT_EQ(call_info(call).phase, AYNI_CALL_QUEUED);
    ASSERT_EQ(append.batches.size(), 1U);
    EXPECT_TRUE(pending.empty());
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
    const auto task = ready_task(create());
    ASSERT_NE(task, 0U);
    uint64_t call = 0;
    {
        CallAppendGroup append(work.node());
        work.node().step();
        call = work.task(task).logical_call_id;
        ASSERT_NE(call, 0U);
        ASSERT_EQ(call_info(call).phase, AYNI_CALL_DISPATCHING);
        ASSERT_EQ(append.batches.size(), 3U);
        const ayni_grant_revoke_v1 revoke{sizeof(revoke), 0, grants[0]};
        auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &revoke, 1);
        intent.command_id = ++command;
        ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    }
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
                                     nullptr,
                                     {},
                                     {},
                                     {}};
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
                                            nullptr,
                                            {},
                                            {},
                                            {}};
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
        TextValue input(text("question"));
        const ayni_call_request_v1 request{sizeof(request),
                                           AYNI_POLICY_REQUEST_MODEL,
                                           1,
                                           0,
                                           {key.data(), static_cast<uint32_t>(key.size())},
                                           text("mock.echo"),
                                           {},
                                           {},
                                           input.view(),
                                           AYNI_CALL_MODE_FOREGROUND,
                                           0};
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
                                         nullptr,
                                         {},
                                         {},
                                         {}};
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
