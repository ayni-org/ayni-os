// ADR-0057: public Native usage, normal orderly reopen, and deterministic Sim wiring.
#include "loopback_http.h"
#include "temp_directory.h"
#include <ayni/host_authority.h>
#include <ayni/runtime/native_node/assistant.h>
#include <ayni/runtime/sim_node.h>
#include <chrono>
#include <cstdlib>
#include <gtest/gtest.h>
#include <string>
#include <vector>
namespace {
using namespace ayni::runtime;
class NativeAssistant : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
        settle();
    }
    void TearDown() override { EXPECT_EQ(app.close(), AYNI_OK); }
    void settle() {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            ASSERT_EQ(app.advance(notices), AYNI_OK);
            if (app.idle()) {
                return;
            }
            app.wait();
        } while (std::chrono::steady_clock::now() < end);
        FAIL() << "runtime did not become idle";
    }
    Reply run(Operation op, uint64_t id = 0, const std::string &text = {},
              uint64_t agent = demo_agent_b) {
        Command command;
        command.operation = op;
        command.id = id;
        command.text = text;
        command.agent = agent;
        return app.execute(command);
    }
    std::string conversation() {
        const auto reply = run(Operation::conversation);
        EXPECT_EQ(reply.status, AYNI_OK);
        std::string out = reply.conversation.summary;
        for (const auto &turn : reply.conversation.turns) {
            out += turn.user + turn.answer;
        }
        return out;
    }
    void allow(uint64_t agent, const char *cap) {
        const auto reply = run(Operation::grant, 0, cap, agent);
        ASSERT_EQ(reply.status, AYNI_OK);
        settle();
    }
    void allow_chat() {
        for (const auto cap : {"model", "summary", "present"}) {
            allow(demo_agent_b, cap);
        }
    }
    ayni::test::TempDirectory directory;
    Assistant app;
    std::vector<Notice> notices;
};
TEST_F(NativeAssistant, rule_and_chat_agents_use_separate_owned_work) {
    allow(demo_agent_a, "present");
    allow_chat();
    ASSERT_EQ(run(Operation::say, 0, "check", demo_agent_a).status, AYNI_OK);
    ASSERT_EQ(run(Operation::say, 0, "你好").status, AYNI_OK);
    settle();
    EXPECT_EQ(run(Operation::tasks).tasks.front().lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_NE(conversation().find("你好"), std::string::npos);
    EXPECT_EQ(notices.size(), 2U);
    EXPECT_FALSE(run(Operation::calls).calls.empty());
}
TEST_F(NativeAssistant, summary_and_conversation_survive_clean_close_and_reopen) {
    allow_chat();
    for (const auto input : {"第一轮", "第二轮", "第三轮", "第四轮"}) {
        ASSERT_EQ(run(Operation::say, 0, input).status, AYNI_OK);
        settle();
    }
    const auto before = conversation();
    EXPECT_FALSE(run(Operation::conversation).conversation.summary.empty());
    EXPECT_EQ(before.find("第一轮"), std::string::npos);
    EXPECT_NE(before.find("第四轮"), std::string::npos);
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
    settle();
    EXPECT_EQ(conversation(), before);
    ASSERT_EQ(run(Operation::say, 0, "第五轮").status, AYNI_OK);
    settle();
    EXPECT_NE(conversation().find("第五轮"), std::string::npos);
}
TEST_F(NativeAssistant, proposed_turn_can_be_approved_after_normal_reopen) {
    const auto proposed = run(Operation::say, 0, "需要批准");
    ASSERT_EQ(proposed.status, AYNI_OK);
    settle();
    EXPECT_EQ(run(Operation::goals).goals.front().lifecycle, AYNI_GOAL_PROPOSED);
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::approve, proposed.id).status, AYNI_OK);
    settle();
    EXPECT_NE(conversation().find("需要批准"), std::string::npos);
}
TEST_F(NativeAssistant, rule_agent_restores_its_pending_goal_and_reports_completion) {
    const auto proposed = run(Operation::say, 0, "check", demo_agent_a);
    ASSERT_EQ(proposed.status, AYNI_OK);
    settle();
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
    settle();
    EXPECT_EQ(run(Operation::say, 0, "check", demo_agent_a).status, AYNI_BUSY);
    ASSERT_EQ(run(Operation::approve, proposed.id).status, AYNI_OK);
    settle();
    ASSERT_FALSE(notices.empty());
    EXPECT_EQ(notices.back().kind, NoticeKind::rule_completed);
}
TEST_F(NativeAssistant, whole_context_delete_is_saved_and_does_not_erase_kernel_history) {
    allow_chat();
    ASSERT_EQ(run(Operation::say, 0, "旧上下文").status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::erase).status, AYNI_OK);
    settle();
    EXPECT_EQ(conversation().find("旧上下文"), std::string::npos);
    EXPECT_EQ(run(Operation::goals).goals.front().lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
    settle();
    EXPECT_EQ(conversation().find("旧上下文"), std::string::npos);
}
TEST_F(NativeAssistant, grant_revoke_and_cancel_use_public_management_commands) {
    auto grant = run(Operation::grant, 0, "model");
    ASSERT_EQ(grant.status, AYNI_OK);
    EXPECT_NE(grant.command_id, 0U);
    EXPECT_EQ(grant.phase, AYNI_INTENT_APPLIED);
    EXPECT_FALSE(app.idle());
    settle();
    EXPECT_EQ(run(Operation::status).pending_commands, 0U);
    const auto revoked = run(Operation::revoke, grant.id);
    ASSERT_EQ(revoked.status, AYNI_OK);
    EXPECT_NE(revoked.command_id, grant.command_id);
    EXPECT_NE(revoked.phase, 0U);
    settle();
    EXPECT_EQ(run(Operation::status).pending_commands, 0U);
    auto goal = run(Operation::say, 0, "先取消");
    ASSERT_EQ(goal.status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::cancel, goal.id).status, AYNI_OK);
    settle();
    EXPECT_EQ(run(Operation::goals).goals.front().lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(conversation().find("先取消"), std::string::npos);
    EXPECT_EQ(run(Operation::say, 0, std::string(385, 'x')).status, AYNI_CAPACITY_EXCEEDED);
}
TEST_F(NativeAssistant,
       continuing_past_retained_capacity_keeps_latest_context_and_queryable_calls) {
    allow_chat();
    constexpr unsigned turns = 22;
    for (unsigned at = 1; at <= turns; ++at) {
        ASSERT_EQ(run(Operation::say, 0, "turn-" + std::to_string(at)).status, AYNI_OK);
        settle();
    }
    EXPECT_EQ(run(Operation::status).last_round, turns);
    EXPECT_NE(conversation().find("turn-22"), std::string::npos);
    EXPECT_EQ(run(Operation::goals).goals.size(), demo_config().goal_capacity);
    const auto calls = run(Operation::calls);
    ASSERT_EQ(calls.status, AYNI_OK);
    ASSERT_FALSE(calls.calls.empty());
    for (const auto &call : calls.calls) {
        EXPECT_EQ(call.outcome, AYNI_CALL_COMPLETED);
    }
}
TEST_F(NativeAssistant, local_http_tool_loop_and_text_only_summary_use_real_provider_wiring) {
    ASSERT_EQ(app.close(), AYNI_OK);
    const std::string answer =
        R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"已处理"}}]})";
    const std::string tool =
        R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"fixture","type":"function","function":{"name":"tool_0","arguments":"{\"text\":\"模拟输出\"}"}}]}}]})";
    ayni::test::LoopbackHttp server(answer, 200, 0, {}, [&](const std::string &, unsigned count) {
        return count == 1 ? tool : answer;
    });
    ASSERT_EQ(setenv("AYNI_P2_FIXTURE_KEY", "local-fixture-only", 1), 0);
    ModelConfig config;
    config.model = Model::remote;
    config.base_url = server.url();
    config.model_name = "fixture";
    config.key_environment = "AYNI_P2_FIXTURE_KEY";
    ASSERT_EQ(app.open(directory.path(), config), AYNI_OK);
    settle();
    allow_chat();
    for (const auto input : {"工具调用", "第二轮", "第三轮", "第四轮"}) {
        ASSERT_EQ(run(Operation::say, 0, input).status, AYNI_OK);
        settle();
        for (const auto &task : run(Operation::tasks).tasks) {
            ASSERT_EQ(task.lifecycle, AYNI_TASK_COMPLETED)
                << "hold=" << task.hold_status << ':' << task.hold_reason;
        }
    }
    ASSERT_EQ(server.requests(), 6U);
    const auto requests = server.history();
    ASSERT_EQ(requests.size(), 6U);
    EXPECT_NE(requests[0].find("\"tools\""), std::string::npos);
    EXPECT_EQ(requests[4].find("\"tools\""), std::string::npos);
    EXPECT_NE(requests[4].find("Summarize"), std::string::npos);
    EXPECT_EQ(run(Operation::conversation).conversation.summary, "已处理");
    ASSERT_EQ(app.close(), AYNI_OK);
    unsetenv("AYNI_P2_FIXTURE_KEY");
}
TEST(SimDemo, independent_nodes_use_the_same_profile_and_retain_storage_on_release) {
    SimNode first, second;
    for (uint64_t expected_epoch = 1; expected_epoch <= 2; ++expected_epoch) {
        for (auto *node : {&first, &second}) {
            DemoProfile profile;
            ASSERT_EQ(node->assemble(demo_config()), AYNI_OK);
            ASSERT_EQ(profile.assemble(node->kernel(), {}), AYNI_OK);
            ASSERT_EQ(ayni_start(node->kernel()), AYNI_OK);
            for (unsigned i = 0; i < 200; ++i) {
                ayni_step_report_v1 report{};
                report.struct_size = sizeof(report);
                ASSERT_EQ(ayni_step(node->kernel(), 64, 10000, &report), AYNI_OK);
                node->advance(1000);
            }
            ayni_kernel_info_v1 info{};
            info.struct_size = sizeof(info);
            ASSERT_EQ(ayni_get_kernel_info(node->kernel(), &info), AYNI_OK);
            EXPECT_EQ(info.boot_epoch, expected_epoch);
            ASSERT_EQ(ayni_begin_shutdown(node->kernel()), AYNI_OK);
            for (unsigned i = 0; i < 200; ++i) {
                ayni_step_report_v1 report{};
                report.struct_size = sizeof(report);
                ayni_lifecycle_t state = 0;
                ASSERT_EQ(ayni_get_lifecycle(node->kernel(), &state), AYNI_OK);
                if (state == AYNI_LIFECYCLE_STOPPED) {
                    break;
                }
                ASSERT_EQ(ayni_step(node->kernel(), 64, 10000, &report), AYNI_OK);
                node->advance(1000);
            }
            ASSERT_EQ(node->release(), AYNI_OK);
            ASSERT_EQ(profile.close(), AYNI_OK);
        }
    }
}
} // namespace
