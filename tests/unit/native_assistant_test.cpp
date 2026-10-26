// ADR-0057: public Native usage, normal orderly reopen, and deterministic Sim wiring.
#include "loopback_http.h"
#include "temp_directory.h"
#include <ayni/host_authority.h>
#include <ayni/runtime/native_node/assistant.h>
#include <ayni/runtime/sim_node.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <string>
#include <vector>
namespace {
using namespace ayni::runtime;
// ADR-0060 requires durable completion, not a five-second wall-clock SLA. Leave
// room for sanitizer overhead and concurrent fsync-heavy Host tests; a stall
// still fails at this finite deadline with the outstanding public state.
constexpr auto async_test_timeout = std::chrono::seconds(30);
class NativeAssistant : public ::testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
        settle();
    }
    void TearDown() override { EXPECT_EQ(app.close(), AYNI_OK); }
    void settle() {
        const auto end = std::chrono::steady_clock::now() + async_test_timeout;
        do {
            ASSERT_EQ(app.advance(notices), AYNI_OK);
            if (app.idle()) {
                return;
            }
            app.wait();
        } while (std::chrono::steady_clock::now() < end);
        const auto state = run(Operation::status);
        FAIL() << "runtime did not become idle: status=" << state.status
               << ", durable_seq=" << state.kernel.durable_seq
               << ", context_seq=" << state.context.commit_seq
               << ", context_action=" << state.context.pending_action
               << ", privacy_phase=" << state.privacy.phase
               << ", pending_goal=" << state.pending_goal
               << ", pending_commands=" << state.pending_commands << ", saving=" << state.saving
               << ", storage_status=" << state.storage_status;
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
TEST_F(NativeAssistant, whole_context_delete_erases_content_and_preserves_actual_work_outcomes) {
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
// ADR-0060: deletion leaves sparse user turns, destroys derived answers/summary, and never reuses
// IDs.
TEST_F(NativeAssistant,
       deleting_one_turn_preserves_other_users_and_rebuilds_summary_from_valid_inputs) {
    allow_chat();
    for (const auto input : {"留第一轮", "删第二轮", "留第三轮"}) {
        ASSERT_EQ(run(Operation::say, 0, input).status, AYNI_OK);
        settle();
    }
    ASSERT_EQ(run(Operation::erase, 2).status, AYNI_OK);
    settle();
    auto view = run(Operation::conversation).conversation;
    ASSERT_EQ(view.turns.size(), 2U);
    EXPECT_EQ(view.turns[0].number, 1U);
    EXPECT_EQ(view.turns[1].number, 3U);
    EXPECT_TRUE(view.turns[0].answer.empty());
    EXPECT_TRUE(view.turns[1].answer.empty());
    EXPECT_TRUE(view.summary.empty());
    EXPECT_EQ(conversation().find("删第二轮"), std::string::npos);
    const auto state = run(Operation::status);
    EXPECT_EQ(state.privacy.phase, AYNI_PRIVACY_COMPLETE);
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
    settle();
    for (const auto input : {"第四轮", "第五轮"}) {
        ASSERT_EQ(run(Operation::say, 0, input).status, AYNI_OK);
        settle();
    }
    view = run(Operation::conversation).conversation;
    EXPECT_FALSE(view.summary.empty());
    EXPECT_EQ(run(Operation::status).last_round, 5U);
    EXPECT_EQ(conversation().find("删第二轮"), std::string::npos);
}
// ADR-0060: explicit memory selection reaches the policy view; deleting the source clears
// its conversation derivatives and all managed on-disk copies, while keeping user text.
TEST_F(NativeAssistant, memory_save_use_delete_clears_generated_context_and_managed_files) {
    allow_chat();
    const std::string secret = "p3-private-memory-marker";
    const auto saved = run(Operation::memory_save, 0, secret);
    ASSERT_EQ(saved.status, AYNI_OK);
    settle();
    const auto shown = run(Operation::memory_show, saved.id);
    ASSERT_EQ(shown.status, AYNI_OK);
    ASSERT_EQ(shown.memories.size(), 1U);
    EXPECT_EQ(shown.memories.front().body, secret);
    EXPECT_EQ(shown.memories.front().info.confirmed, 1U);
    ASSERT_EQ(run(Operation::say, saved.id, "使用已选记忆").status, AYNI_OK);
    settle();
    ASSERT_FALSE(run(Operation::conversation).conversation.turns.front().answer.empty());
    ASSERT_EQ(run(Operation::memory_delete, saved.id).status, AYNI_OK);
    settle();
    const auto view = run(Operation::conversation).conversation;
    ASSERT_EQ(view.turns.size(), 1U);
    EXPECT_EQ(view.turns.front().user, "使用已选记忆");
    EXPECT_TRUE(view.turns.front().answer.empty());
    EXPECT_EQ(run(Operation::memory_show, saved.id).status, AYNI_NOT_FOUND);
    EXPECT_EQ(run(Operation::status).privacy.phase, AYNI_PRIVACY_COMPLETE);
    for (const auto &entry : std::filesystem::directory_iterator(directory.path())) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream input(entry.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        EXPECT_EQ(bytes.find(secret), std::string::npos) << entry.path();
    }
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
    settle();
    EXPECT_EQ(run(Operation::memory_show, saved.id).status, AYNI_NOT_FOUND);
    EXPECT_TRUE(run(Operation::conversation).conversation.turns.front().answer.empty());
}
// ADR-0060: a superseded publication removes model output, never the independent user input.
TEST_F(NativeAssistant, invalidating_publication_preserves_draft_across_reopen_and_next_turn) {
    allow_chat();
    const auto saved = run(Operation::memory_save, 0, "private-source-for-publishing");
    ASSERT_EQ(saved.status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::say, saved.id, "保留本轮原始提问").status, AYNI_OK);
    bool publishing = false;
    const auto end = std::chrono::steady_clock::now() + async_test_timeout;
    do {
        ASSERT_EQ(app.advance(notices), AYNI_OK);
        const auto status = run(Operation::status);
        ASSERT_EQ(status.status, AYNI_OK);
        if (status.context.pending_action == AYNI_CONTEXT_PUBLISH && !status.saving) {
            publishing = true;
            break;
        }
        app.wait();
    } while (std::chrono::steady_clock::now() < end);
    ASSERT_TRUE(publishing);
    ASSERT_EQ(run(Operation::memory_delete, saved.id).status, AYNI_OK);
    settle();
    auto view = run(Operation::conversation).conversation;
    ASSERT_EQ(view.turns.size(), 1U);
    EXPECT_EQ(view.turns.front().user, "保留本轮原始提问");
    EXPECT_TRUE(view.turns.front().answer.empty());
    EXPECT_TRUE(view.draft.empty());
    EXPECT_EQ(run(Operation::status).last_round, 1U);
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
    settle();
    EXPECT_EQ(run(Operation::conversation).conversation.turns.front().user,
              view.turns.front().user);
    ASSERT_EQ(run(Operation::say, 0, "继续回答").status, AYNI_OK);
    settle();
    view = run(Operation::conversation).conversation;
    ASSERT_EQ(view.turns.size(), 2U);
    EXPECT_EQ(view.turns.front().number, 1U);
    EXPECT_EQ(view.turns.back().number, 2U);
    EXPECT_EQ(view.turns.back().user, "继续回答");
    EXPECT_TRUE(view.draft.empty());
    ASSERT_EQ(run(Operation::erase, 1).status, AYNI_OK);
    settle();
    EXPECT_EQ(conversation().find("保留本轮原始提问"), std::string::npos);
    EXPECT_NE(conversation().find("继续回答"), std::string::npos);
    for (const auto &grant : run(Operation::grants).grants) {
        ASSERT_EQ(run(Operation::revoke, grant.grant_id).status, AYNI_OK);
        settle();
    }
    ASSERT_EQ(run(Operation::say, 0, "新的未完成输入").status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::erase, 2).status, AYNI_OK);
    settle();
    view = run(Operation::conversation).conversation;
    EXPECT_TRUE(view.turns.empty());
    EXPECT_EQ(view.draft, "新的未完成输入");
}
// ADR-0060: sharing is per source and per Agent; revocation removes already captured context too.
TEST_F(NativeAssistant, sharing_private_memory_then_revoking_blocks_reads_and_old_context) {
    allow_chat();
    const auto saved = run(Operation::memory_save, 0, "A 的私有偏好", demo_agent_a);
    ASSERT_EQ(saved.status, AYNI_OK);
    settle();
    const auto denied = run(Operation::say, saved.id, "读取 A 的偏好");
    ASSERT_EQ(denied.status, AYNI_OK);
    settle();
    auto tasks = run(Operation::tasks).tasks;
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(tasks.front().hold_reason, AYNI_REASON_MEMORY_SOURCE);
    EXPECT_TRUE(run(Operation::calls).calls.empty());
    ASSERT_EQ(run(Operation::cancel, denied.id).status, AYNI_OK);
    settle();
    const auto grant = run(Operation::memory_share, saved.id, {}, demo_agent_b);
    ASSERT_EQ(grant.status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::say, saved.id, "现在读取共享偏好").status, AYNI_OK);
    settle();
    ASSERT_FALSE(run(Operation::conversation).conversation.turns.front().answer.empty());
    ASSERT_EQ(run(Operation::revoke, grant.id).status, AYNI_OK);
    settle();
    auto view = run(Operation::conversation).conversation;
    ASSERT_EQ(view.turns.size(), 1U);
    EXPECT_EQ(view.turns.front().user, "读取 A 的偏好\n现在读取共享偏好");
    EXPECT_TRUE(view.turns.front().answer.empty());
    const auto blocked = run(Operation::say, saved.id, "撤销后读取");
    ASSERT_EQ(blocked.status, AYNI_OK);
    settle();
    tasks = run(Operation::tasks).tasks;
    EXPECT_EQ(tasks.back().hold_reason, AYNI_REASON_MEMORY_SOURCE);
    ASSERT_EQ(run(Operation::cancel, blocked.id).status, AYNI_OK);
    settle();
}
TEST_F(NativeAssistant, profile_starts_empty_and_grant_to_a_does_not_authorize_b) {
    allow_chat();
    EXPECT_TRUE(run(Operation::profile_list).memories.empty());
    Command save;
    save.operation = Operation::profile_save;
    save.profile_field = AYNI_PROFILE_LANGUAGE;
    save.text = "中文";
    const auto saved = app.execute(save);
    ASSERT_EQ(saved.status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::memory_share, saved.id, {}, demo_agent_a).status, AYNI_OK);
    settle();
    const auto denied = run(Operation::say, saved.id, "使用档案语言");
    ASSERT_EQ(denied.status, AYNI_OK);
    settle();
    const auto tasks = run(Operation::tasks).tasks;
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().hold_reason, AYNI_REASON_MEMORY_SOURCE);
    EXPECT_TRUE(run(Operation::calls).calls.empty());
    ASSERT_EQ(run(Operation::cancel, denied.id).status, AYNI_OK);
    settle();
}
// ADR-0060: a legacy file has no Kernel source evidence; refusal precedes every boot write.
TEST_F(NativeAssistant, legacy_conversation_refusal_preserves_the_entire_kernel_directory) {
    ASSERT_EQ(app.close(), AYNI_OK);
    const auto path = std::filesystem::path(directory.path()) / "conversation.bin";
    std::ifstream input(path, std::ios::binary);
    std::string legacy((std::istreambuf_iterator<char>(input)), {});
    input.close();
    ASSERT_GT(legacy.size(), 40U);
    legacy[4] = 1; // AYNF v1: no context ID at bytes 24..31.
    legacy.erase(24, 8);
    legacy[28] = 1; // AYNC v1 record version after the old 24-byte outer header.
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(legacy.data(), static_cast<std::streamsize>(legacy.size()));
    }
    std::map<std::filesystem::path, std::string> before;
    for (const auto &entry : std::filesystem::directory_iterator(directory.path())) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream file(entry.path(), std::ios::binary);
        before.emplace(entry.path(), std::string((std::istreambuf_iterator<char>(file)), {}));
    }
    EXPECT_EQ(app.open(directory.path(), {}), AYNI_UNSUPPORTED_VERSION);
    for (const auto &entry : before) {
        std::ifstream file(entry.first, std::ios::binary);
        const std::string after((std::istreambuf_iterator<char>(file)), {});
        EXPECT_EQ(after, entry.second) << entry.first;
    }
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
    EXPECT_EQ(run(Operation::conversation).conversation.draft, "先取消");
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
// ADR-0060: memory ownership/read access does not imply permission to send it to a model endpoint.
TEST_F(NativeAssistant, remote_memory_requires_separate_destination_grant_before_http_submission) {
    ASSERT_EQ(app.close(), AYNI_OK);
    const std::string answer =
        R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"已处理"}}]})";
    ayni::test::LoopbackHttp server(answer);
    ASSERT_EQ(setenv("AYNI_P3_FIXTURE_KEY", "local-fixture-only", 1), 0);
    ModelConfig config;
    config.model = Model::remote;
    config.base_url = server.url();
    config.model_name = "fixture";
    config.key_environment = "AYNI_P3_FIXTURE_KEY";
    ASSERT_EQ(app.open(directory.path(), config), AYNI_OK);
    settle();
    allow_chat();
    const std::string secret = "memory-to-model-with-explicit-permission";
    const auto saved = run(Operation::memory_save, 0, secret);
    ASSERT_EQ(saved.status, AYNI_OK);
    settle();
    const auto denied = run(Operation::say, saved.id, "使用记忆");
    ASSERT_EQ(denied.status, AYNI_OK);
    settle();
    EXPECT_EQ(server.requests(), 0U);
    ASSERT_EQ(run(Operation::cancel, denied.id).status, AYNI_OK);
    settle();
    const auto grant = run(Operation::memory_export, saved.id, {}, demo_agent_b);
    ASSERT_EQ(grant.status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::say, saved.id, "明确允许外发").status, AYNI_OK);
    settle();
    EXPECT_EQ(server.requests(), 1U);
    EXPECT_NE(server.request().find(secret), std::string::npos);
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), config), AYNI_OK);
    settle();
    EXPECT_NE(conversation().find("明确允许外发"), std::string::npos);
    ASSERT_EQ(run(Operation::revoke, grant.id).status, AYNI_OK);
    settle();
    EXPECT_TRUE(run(Operation::conversation).conversation.turns.front().answer.empty());
    ASSERT_EQ(app.close(), AYNI_OK);
    EXPECT_EQ(unsetenv("AYNI_P3_FIXTURE_KEY"), 0);
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
