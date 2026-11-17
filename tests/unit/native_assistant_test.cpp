// ADR-0057: public Native usage, normal orderly reopen, and deterministic Sim wiring.
#include "loopback_http.h"
#include "temp_directory.h"
#include <algorithm>
#include <ayni/applications/assistant/schedule_tool.h>
#include <ayni/host_authority.h>
#include <ayni/runtime/common/memory_bodies.h>
#include <ayni/runtime/native_node/assistant.h>
#include <ayni/runtime/sim_node.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <sqlite3.h>
#include <string>
#include <vector>
namespace {
using namespace ayni::runtime;
// ADR-0060 requires durable completion, not a five-second wall-clock SLA. Leave
// room for sanitizer overhead and concurrent fsync-heavy Host tests; a stall
// still fails at this finite deadline with the outstanding public state.
constexpr auto async_test_timeout = std::chrono::seconds(30);
// conversation.bin starts with "AYNF" and then its little-endian format version.
constexpr size_t format_version_offset = 4;
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
    Reply run(Operation op, uint64_t id = 0, const std::string &text = {}) {
        Command command;
        command.operation = op;
        command.id = id;
        command.text = text;
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
    void allow(const char *cap) {
        const auto reply = run(Operation::grant, 0, cap);
        ASSERT_EQ(reply.status, AYNI_OK);
        settle();
    }
    void allow_chat() {
        for (const auto cap : {"model", "summary", "schedule"}) {
            allow(cap);
        }
    }
    const ayni_goal_info_v1 *find(const std::vector<ayni_goal_info_v1> &goals, uint64_t id) {
        const auto found = std::find_if(goals.begin(), goals.end(),
                                        [&](const auto &goal) { return goal.goal_id == id; });
        return found == goals.end() ? nullptr : &*found;
    }
    // Opening must refuse before boot writes anything: every regular file stays byte-identical.
    void expect_refused_without_writes() {
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
    std::string conversation_file() const {
        return (std::filesystem::path(directory.path()) / "conversation.bin").string();
    }
    ayni::test::TempDirectory directory;
    Assistant app;
    std::vector<Notice> notices;
};
// C-NATIVE-001/002: startup must finish local loading without waiting for owner approval.
TEST(NativeStartup, loading_stays_visible_until_the_original_turn_or_record_is_available) {
    ayni::test::TempDirectory directory;
    Assistant app;
    std::vector<Notice> notices;
    Command status;
    status.operation = Operation::status;
    Command say;
    say.operation = Operation::say;
    say.text = "startup draft";
    uint64_t original_goal = 0;
    for (uint32_t opening = 0; opening < 2U; ++opening) {
        ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
        auto state = app.execute(status);
        ASSERT_EQ(state.status, AYNI_OK);
        EXPECT_FALSE(state.context_ready); // The first root read has not been admitted yet.
        const auto deadline = std::chrono::steady_clock::now() + async_test_timeout;
        while (!state.context_ready && std::chrono::steady_clock::now() < deadline) {
            ASSERT_EQ(app.advance(notices), AYNI_OK);
            app.wait();
            state = app.execute(status);
            ASSERT_EQ(state.status, AYNI_OK);
        }
        ASSERT_TRUE(state.context_ready);
        if (!original_goal) {
            Command history;
            history.operation = Operation::conversation;
            EXPECT_EQ(app.execute(history).status, AYNI_OK);
            const auto accepted = app.execute(say);
            ASSERT_EQ(accepted.status, AYNI_OK);
            original_goal = accepted.id;
            ASSERT_NE(original_goal, 0U);
        } else {
            EXPECT_EQ(state.pending_goal, original_goal);
            Command goals;
            goals.operation = Operation::goals;
            const auto view = app.execute(goals);
            ASSERT_EQ(view.status, AYNI_OK);
            ASSERT_EQ(view.goals.size(), 1U);
            EXPECT_EQ(view.goals.front().goal_id, original_goal);
            EXPECT_EQ(view.goals.front().lifecycle, AYNI_GOAL_PROPOSED);
        }
        ASSERT_EQ(app.close(), AYNI_OK);
    }
}
// ADR-0072: one assistant; "check" is ordinary text for its model turn, not a rule route.
TEST_F(NativeAssistant, check_text_is_an_ordinary_turn_of_the_single_assistant) {
    allow_chat();
    ASSERT_EQ(run(Operation::say, 0, "check").status, AYNI_OK);
    settle();
    const auto goals = run(Operation::goals).goals;
    ASSERT_EQ(goals.size(), 1U);
    EXPECT_EQ(std::string(goals.front().goal_type_id, goals.front().goal_type_id_size), "chat");
    EXPECT_EQ(run(Operation::tasks).tasks.front().lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_NE(conversation().find("check"), std::string::npos);
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices.front().kind, NoticeKind::answer);
    EXPECT_FALSE(run(Operation::calls).calls.empty());
}
// ADR-0085: Native composition exposes Shell only with explicit owner authority;
// a normal pipeline uses the ordinary Call path and retains its original result.
TEST_F(NativeAssistant, shell_pipeline_requires_a_grant_and_retains_its_original_typed_body) {
    ASSERT_EQ(app.close(), AYNI_OK);
    ModelConfig config;
    config.shell_working_directory = directory.path();
    ASSERT_EQ(app.open(directory.path(), config), AYNI_OK);
    settle();
    allow_chat();
    const std::string command =
        R"(function:ayni_shell json:{"command":"printf hello | tr a-z A-Z"})";
    ASSERT_EQ(run(Operation::say, 0, command).status, AYNI_OK);
    settle();
    auto calls = run(Operation::calls).calls;
    const auto first = calls.size();
    EXPECT_EQ(first, 2U); // Two Model Calls surround the local refusal.
    allow("shell");
    ASSERT_EQ(run(Operation::say, 0, command).status, AYNI_OK);
    settle();
    calls = run(Operation::calls).calls;
    EXPECT_EQ(calls.size(), first + 3U);
    const auto business = std::find_if(calls.begin(), calls.end(), [](const auto &call) {
        return call.result_kind == AYNI_CALL_RESULT_VALUE && call.outcome == AYNI_CALL_COMPLETED;
    });
    ASSERT_NE(business, calls.end());
    EXPECT_EQ(business->effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_NE(business->body.id, 0U);
    sqlite3 *db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(directory.file("conversation.sqlite3").c_str(), &db,
                              SQLITE_OPEN_READONLY, nullptr),
              SQLITE_OK);
    sqlite3_stmt *query = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db, "SELECT body FROM state", -1, &query, nullptr), SQLITE_OK);
    bool original_saved = false;
    while (sqlite3_step(query) == SQLITE_ROW) {
        const std::string bytes(static_cast<const char *>(sqlite3_column_blob(query, 0)),
                                size_t(sqlite3_column_bytes(query, 0)));
        original_saved |= bytes.find("SEVMTE8=") != std::string::npos;
    }
    EXPECT_TRUE(
        original_saved); // The typed stdout field retains base64(HELLO), without a JSON archive.
    sqlite3_finalize(query);
    sqlite3_close(db);
}
// ADR-0066: grouped autonomous Goal echoes retain immutable membership during approval.
TEST_F(NativeAssistant, autonomous_goals_reopen_and_approve_through_the_public_application) {
    Command check;
    check.operation = Operation::schedule;
    check.text = "check";
    const auto first = app.execute(check);
    ASSERT_EQ(first.status, AYNI_OK);
    check.text = "check 600";
    const auto second = app.execute(check);
    ASSERT_EQ(second.status, AYNI_OK);
    ASSERT_NE(first.id, second.id);
    settle();
    ASSERT_EQ(app.close(), AYNI_OK);
    ASSERT_EQ(app.open(directory.path(), {}), AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::approve, first.id).status, AYNI_OK);
    settle();
    const auto goals = run(Operation::goals);
    const auto found = std::find_if(goals.goals.begin(), goals.goals.end(),
                                    [&](const auto &g) { return g.goal_id == first.id; });
    ASSERT_NE(found, goals.goals.end());
    EXPECT_EQ(found->pause_group, ayni::applications::assistant::autonomous_group);
    EXPECT_EQ(found->lifecycle, AYNI_GOAL_ACTIVE);
}
TEST_F(NativeAssistant, short_history_and_conversation_survive_clean_close_and_reopen) {
    allow_chat();
    for (const auto input : {"第一轮", "第二轮", "第三轮", "第四轮"}) {
        ASSERT_EQ(run(Operation::say, 0, input).status, AYNI_OK);
        settle();
    }
    const auto before = conversation();
    EXPECT_TRUE(run(Operation::conversation).conversation.summary.empty());
    EXPECT_NE(before.find("第一轮"), std::string::npos);
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
// ADR-0072: a stop phrase never reaches a model. It cancels the pending turn and pauses the
// autonomous group while reminders stay; a repeated stop has nothing left to submit.
TEST_F(NativeAssistant, stop_phrase_cancels_the_turn_pauses_autonomy_and_keeps_reminders) {
    allow("notify");
    Command schedule;
    schedule.operation = Operation::schedule;
    schedule.text = "remind daily 2030-01-01T08:00 UTC0 喝水";
    const auto reminder = app.execute(schedule);
    ASSERT_EQ(reminder.status, AYNI_OK);
    schedule.text = "check";
    const auto check = app.execute(schedule);
    ASSERT_EQ(check.status, AYNI_OK);
    const auto turn = run(Operation::say, 0, "等待批准的一轮");
    ASSERT_EQ(turn.status, AYNI_OK);
    settle();
    const auto stopped = run(Operation::say, 0, " 停！");
    ASSERT_EQ(stopped.status, AYNI_OK);
    EXPECT_EQ(stopped.message, Message::stopped);
    settle();
    const auto goals = run(Operation::goals).goals;
    ASSERT_NE(find(goals, turn.id), nullptr);
    EXPECT_EQ(find(goals, turn.id)->lifecycle, AYNI_GOAL_CANCELLED);
    ASSERT_NE(find(goals, reminder.id), nullptr);
    EXPECT_EQ(find(goals, reminder.id)->lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(find(goals, reminder.id)->pause_group, 0U);
    EXPECT_FALSE(find(goals, reminder.id)->group_paused);
    ASSERT_NE(find(goals, check.id), nullptr);
    EXPECT_TRUE(find(goals, check.id)->group_paused);
    const auto state = run(Operation::status);
    EXPECT_TRUE(state.activity.autonomy_paused);
    EXPECT_EQ(state.last_round, 0U);
    EXPECT_TRUE(run(Operation::calls).calls.empty());
    EXPECT_EQ(run(Operation::conversation).conversation.draft, "等待批准的一轮");
    const auto again = run(Operation::stop);
    EXPECT_EQ(again.status, AYNI_OK);
    EXPECT_EQ(again.message, Message::stopped);
    EXPECT_EQ(again.command_id, 0U);
    const auto next = run(Operation::say, 0, "停一下再说");
    ASSERT_EQ(next.status, AYNI_OK);
    EXPECT_EQ(run(Operation::status).pending_goal, next.id);
}
// ADR-0072: stop is admitted while the turn's conversation save is still pending, when every
// other mutation waits.
TEST_F(NativeAssistant, stop_cancels_the_active_shared_session_before_answer_publication) {
    allow_chat();
    const auto turn = run(Operation::say, 0, "马上停止的一轮");
    ASSERT_EQ(turn.status, AYNI_OK);
    ASSERT_EQ(run(Operation::status).pending_goal, turn.id);
    EXPECT_EQ(run(Operation::say, 0, "另一轮").status, AYNI_BUSY);
    const auto stopped = run(Operation::stop);
    ASSERT_EQ(stopped.status, AYNI_OK);
    EXPECT_EQ(stopped.message, Message::stopped);
    settle();
    const auto goals = run(Operation::goals).goals;
    ASSERT_NE(find(goals, turn.id), nullptr);
    EXPECT_EQ(find(goals, turn.id)->lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_TRUE(std::any_of(notices.begin(), notices.end(), [](const Notice &notice) {
        return notice.kind == NoticeKind::turn_aborted;
    }));
    EXPECT_EQ(run(Operation::status).last_round, 0U);
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
       deleting_one_turn_preserves_other_users_and_rebuilds_history_from_valid_inputs) {
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
    EXPECT_TRUE(view.summary.empty()); // Short history remains below the token trigger.
    EXPECT_NE(conversation().find("留第一轮"), std::string::npos);
    EXPECT_NE(conversation().find("留第三轮"), std::string::npos);
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
// ADR-0060: owner profile memory starts empty and is readable only through an explicit share;
// revocation removes already captured context too.
TEST_F(NativeAssistant, sharing_profile_memory_then_revoking_blocks_reads_and_old_context) {
    allow_chat();
    EXPECT_TRUE(run(Operation::profile_list).memories.empty());
    Command save;
    save.operation = Operation::profile_save;
    save.profile_field = AYNI_PROFILE_LANGUAGE;
    save.text = "中文";
    const auto saved = app.execute(save);
    ASSERT_EQ(saved.status, AYNI_OK);
    settle();
    const auto denied = run(Operation::say, saved.id, "读取档案偏好");
    ASSERT_EQ(denied.status, AYNI_OK);
    settle();
    auto tasks = run(Operation::tasks).tasks;
    ASSERT_EQ(tasks.size(), 1U);
    EXPECT_EQ(tasks.front().lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(tasks.front().hold_reason, AYNI_REASON_MEMORY_SOURCE);
    EXPECT_TRUE(run(Operation::calls).calls.empty());
    ASSERT_EQ(run(Operation::cancel, denied.id).status, AYNI_OK);
    settle();
    const auto grant = run(Operation::memory_share, saved.id);
    ASSERT_EQ(grant.status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::say, saved.id, "现在读取共享偏好").status, AYNI_OK);
    settle();
    ASSERT_FALSE(run(Operation::conversation).conversation.turns.front().answer.empty());
    ASSERT_EQ(run(Operation::revoke, grant.id).status, AYNI_OK);
    settle();
    auto view = run(Operation::conversation).conversation;
    ASSERT_EQ(view.turns.size(), 1U);
    EXPECT_EQ(view.turns.front().user, "读取档案偏好\n现在读取共享偏好");
    EXPECT_TRUE(view.turns.front().answer.empty());
    const auto blocked = run(Operation::say, saved.id, "撤销后读取");
    ASSERT_EQ(blocked.status, AYNI_OK);
    settle();
    tasks = run(Operation::tasks).tasks;
    EXPECT_EQ(tasks.back().hold_reason, AYNI_REASON_MEMORY_SOURCE);
    ASSERT_EQ(run(Operation::cancel, blocked.id).status, AYNI_OK);
    settle();
}
// ADR-0060: a legacy file has no Kernel source evidence; refusal precedes every boot write.
TEST_F(NativeAssistant, legacy_conversation_refusal_preserves_the_entire_kernel_directory) {
    ASSERT_EQ(app.close(), AYNI_OK);
    std::string legacy("AYNF");
    legacy.resize(64, '\0');
    legacy[format_version_offset] = 1;
    {
        std::ofstream output(conversation_file(), std::ios::binary | std::ios::trunc);
        output.write(legacy.data(), static_cast<std::streamsize>(legacy.size()));
    }
    expect_refused_without_writes();
}
// ADR-0072: a dual-Agent directory (AYNF v2) is refused before boot, never migrated.
TEST_F(NativeAssistant, dual_agent_conversation_file_is_refused_before_any_boot_write) {
    ASSERT_EQ(app.close(), AYNI_OK);
    std::ofstream file(conversation_file(), std::ios::binary);
    file.write("AYNF", 4);
    file.put(2);
    file.close();
    expect_refused_without_writes();
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
    // ADR-0095: input length is bounded by tokens in the ToolLoop, not by bytes here.
    EXPECT_EQ(run(Operation::say, 0, std::string(385, 'x')).status, AYNI_OK);
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
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"已处理"}}]})";
    ayni::test::LoopbackHttp server(answer);
    ASSERT_EQ(setenv("AYNI_P3_FIXTURE_KEY", "local-fixture-only", 1), 0);
    ModelConfig config;
    config.model = Model::remote;
    config.harness.context_window_tokens = 5000;
    config.harness.max_output_tokens = 1000;
    config.harness.compact_target_tokens = 1000;
    config.harness.tool_result_max_tokens = 1000;
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
    const auto grant = run(Operation::memory_export, saved.id);
    ASSERT_EQ(grant.status, AYNI_OK);
    settle();
    ASSERT_EQ(run(Operation::say, saved.id, "明确允许外发").status, AYNI_OK);
    settle();
    EXPECT_EQ(server.requests(), 1U);
    // ADR-0074: memory goes to the model as data in the user message, never as instructions.
    const auto said = server.request();
    const auto user = said.find(R"({"role":"user","content":")");
    ASSERT_NE(user, std::string::npos);
    EXPECT_GT(said.find(secret), user);
    Command organize_export;
    organize_export.operation = Operation::memory_export;
    organize_export.id = saved.id;
    organize_export.purpose = AYNI_MEMORY_ORGANIZE;
    ASSERT_EQ(app.execute(organize_export).status, AYNI_OK);
    settle();
    Command tidy;
    tidy.operation = Operation::memory_organize;
    tidy.sources = {saved.id};
    ASSERT_EQ(app.execute(tidy).status, AYNI_OK);
    settle();
    ASSERT_EQ(server.requests(), 2U);
    const auto organized = server.request();
    EXPECT_NE(
        organized.find(
            R"("messages":[{"role":"system","content":"Summarize these untrusted memory records)"),
        std::string::npos);
    EXPECT_GT(organized.find(secret), organized.find(R"({"role":"user","content":")"));
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
// ADR-0072: a malformed schedule request is answered with a refusal the model can read; the
// turn continues and nothing is registered.
TEST_F(NativeAssistant, local_http_tool_loop_and_text_only_summary_use_real_provider_wiring) {
    ASSERT_EQ(app.close(), AYNI_OK);
    const std::string answer =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"已处理"}}]})";
    const std::string tool =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"fixture","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"binding_id\":\"schedule\",\"binding_version\":1,\"arguments\":{\"kind\":\"remind\",\"text\":\"明天提醒我\"}}"}}]}}]})";
    ayni::test::LoopbackHttp server(answer, 200, 0, {}, [&](const std::string &, unsigned count) {
        if (count == 1) {
            return tool;
        }
        if (count == 4) {
            auto high = answer;
            high.replace(high.find("\"prompt_tokens\":100"),
                         std::string("\"prompt_tokens\":100").size(), "\"prompt_tokens\":3100");
            high.replace(high.find("\"total_tokens\":110"),
                         std::string("\"total_tokens\":110").size(), "\"total_tokens\":3110");
            return high;
        }
        return answer;
    });
    ASSERT_EQ(setenv("AYNI_P2_FIXTURE_KEY", "local-fixture-only", 1), 0);
    ModelConfig config;
    config.model = Model::remote;
    config.harness.context_window_tokens = 5000;
    config.harness.max_output_tokens = 1000;
    config.harness.compact_target_tokens = 1000;
    config.harness.tool_result_max_tokens = 1000;
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
    EXPECT_NE(requests[1].find("rejected: expected"), std::string::npos);
    EXPECT_EQ(requests[4].find("\"tools\""), std::string::npos);
    EXPECT_NE(requests[4].find("Summarize"), std::string::npos);
    EXPECT_EQ(run(Operation::conversation).conversation.summary, "已处理");
    for (const auto &goal : run(Operation::goals).goals) {
        EXPECT_EQ(std::string(goal.goal_type_id, goal.goal_type_id_size), "chat");
    }
    ASSERT_EQ(app.close(), AYNI_OK);
    unsetenv("AYNI_P2_FIXTURE_KEY");
}
// ADR-0072: one model tool call registers one top-level Goal. It waits for the owner even when
// every capability it uses is already granted, and the owner sees text read back from Kernel.
TEST_F(NativeAssistant, model_schedule_tool_registers_a_goal_that_waits_for_owner_approval) {
    ASSERT_EQ(app.close(), AYNI_OK);
    const std::string answer =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"已登记，等你批准"}}]})";
    const std::string tool =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"fixture","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"binding_id\":\"schedule\",\"binding_version\":1,\"arguments\":{\"kind\":\"remind\",\"local_time\":\"2030-01-02T09:00\",\"timezone\":\"UTC0\",\"text\":\"喝水\"}}"}}]}}]})";
    const std::string inspect =
        R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"inspect-schedule","type":"function","function":{"name":"ayni_inspect_capability","arguments":"{\"binding_id\":\"schedule\",\"binding_version\":1}"}}]}}]})";
    ayni::test::LoopbackHttp server(answer, 200, 0, {}, [&](const std::string &, unsigned count) {
        return count == 1 ? inspect : count == 2 ? tool : answer;
    });
    ASSERT_EQ(setenv("AYNI_P7_FIXTURE_KEY", "local-fixture-only", 1), 0);
    ModelConfig config;
    config.model = Model::remote;
    config.harness.context_window_tokens = 32000;
    config.harness.max_output_tokens = 1000;
    config.base_url = server.url();
    config.model_name = "fixture";
    config.key_environment = "AYNI_P7_FIXTURE_KEY";
    config.timezone = "CST-8";
    ASSERT_EQ(app.open(directory.path(), config), AYNI_OK);
    settle();
    allow_chat();
    allow("notify");
    const auto turn = run(Operation::say, 0, "明早九点提醒我喝水");
    ASSERT_EQ(turn.status, AYNI_OK);
    settle();
    ASSERT_EQ(server.requests(), 3U);
    // ADR-0085: stable function definitions precede on-demand business details.
    const auto requests = server.history();
    const auto &first = requests[0];
    ASSERT_NE(first.find("\"tools\":["), std::string::npos);
    EXPECT_NE(first.find("ayni_list_capabilities"), std::string::npos);
    EXPECT_EQ(first.find("YYYY-MM-DDTHH:MM"), std::string::npos);
    EXPECT_NE(requests[1].find("YYYY-MM-DDTHH:MM"), std::string::npos);
    EXPECT_NE(requests[1].find("Resolve relative dates"), std::string::npos);
    EXPECT_NE(requests[1].find("does not run or approve the work"), std::string::npos);
    EXPECT_NE(first.find(
                  R"("messages":[{"role":"system","content":"You are the owner's home assistant.)"),
              std::string::npos);
    EXPECT_NE(first.find("Current local time: "), std::string::npos);
    EXPECT_NE(first.find("Time zone: CST-8"), std::string::npos);
    EXPECT_NE(first.find(R"({"role":"user","content":"明早九点提醒我喝水"})"), std::string::npos);
    EXPECT_NE(requests[2].find("registered"), std::string::npos);
    EXPECT_NE(requests[2].find("waiting_owner"), std::string::npos);
    const auto goals = run(Operation::goals).goals;
    const auto reminder = std::find_if(goals.begin(), goals.end(), [](const auto &goal) {
        return std::string(goal.goal_type_id, goal.goal_type_id_size) == "assistant.reminder";
    });
    ASSERT_NE(reminder, goals.end());
    EXPECT_EQ(reminder->lifecycle, AYNI_GOAL_PROPOSED);
    EXPECT_EQ(reminder->parent_goal_id, 0U);
    ASSERT_NE(find(goals, turn.id), nullptr);
    EXPECT_EQ(find(goals, turn.id)->lifecycle, AYNI_GOAL_SATISFIED);
    const auto proposed = std::find_if(notices.begin(), notices.end(), [](const Notice &notice) {
        return notice.kind == NoticeKind::goal_proposed;
    });
    ASSERT_NE(proposed, notices.end());
    EXPECT_EQ(proposed->id, reminder->goal_id);
    EXPECT_EQ(proposed->body, "remind once 2030-01-02T09:00 UTC0 喝水");
    EXPECT_EQ(std::count_if(
                  notices.begin(), notices.end(),
                  [](const Notice &notice) { return notice.kind == NoticeKind::goal_proposed; }),
              1);
    EXPECT_NE(conversation().find("已登记，等你批准"), std::string::npos);
    ASSERT_EQ(run(Operation::approve, reminder->goal_id).status, AYNI_OK);
    settle();
    EXPECT_EQ(find(run(Operation::goals).goals, reminder->goal_id)->lifecycle, AYNI_GOAL_ACTIVE);
    ASSERT_EQ(app.close(), AYNI_OK);
    unsetenv("AYNI_P7_FIXTURE_KEY");
}
// ADR-0072: every model tool call is a new operation, so the count of model Goals still waiting
// for the owner bounds what one reply can register; the next call is refused as text.
TEST_F(NativeAssistant, model_goals_waiting_for_approval_are_bounded) {
    ASSERT_EQ(app.close(), AYNI_OK);
    const std::string answer =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"有一个没登记上"}}]})";
    std::string tool =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[)";
    constexpr uint32_t calls = ayni::applications::assistant::model_goals_waiting_max + 1;
    for (uint32_t at = 1; at <= calls; ++at) {
        tool +=
            std::string(at == 1 ? "" : ",") + R"({"id":"fixture)" + std::to_string(at) +
            R"(","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"binding_id\":\"schedule\",\"binding_version\":1,\"arguments\":{\"kind\":\"remind\",\"local_time\":\"2030-01-0)" +
            std::to_string(at) + R"(T09:00\",\"timezone\":\"UTC0\",\"text\":\"喝水\"}}"}})";
    }
    tool += "]}}]}";
    ayni::test::LoopbackHttp server(answer, 200, 0, {}, [&](const std::string &, unsigned count) {
        return count == 1 ? tool : answer;
    });
    ASSERT_EQ(setenv("AYNI_P7_FIXTURE_KEY", "local-fixture-only", 1), 0);
    ModelConfig config;
    config.model = Model::remote;
    config.harness.context_window_tokens = 5000;
    config.harness.max_output_tokens = 1000;
    config.harness.compact_target_tokens = 1000;
    config.harness.tool_result_max_tokens = 1000;
    config.base_url = server.url();
    config.model_name = "fixture";
    config.key_environment = "AYNI_P7_FIXTURE_KEY";
    ASSERT_EQ(app.open(directory.path(), config), AYNI_OK);
    settle();
    allow_chat();
    ASSERT_EQ(run(Operation::say, 0, "这几天每天早上提醒我喝水").status, AYNI_OK);
    settle();
    ASSERT_EQ(server.requests(), 2U);
    const auto history = server.history();
    // Count conversation receipts, excluding usage examples in the tool schema.
    const auto results = history[1].substr(0, history[1].find("\"tools\":["));
    const auto occurrences = [&](const std::string &needle) {
        size_t count = 0;
        for (auto at = results.find(needle); at != std::string::npos;
             at = results.find(needle, at + needle.size())) {
            ++count;
        }
        return count;
    };
    EXPECT_EQ(occurrences("waiting_owner"), ayni::applications::assistant::model_goals_waiting_max);
    EXPECT_EQ(occurrences("earlier goals still wait for owner approval"), 1U);
    const auto goals = run(Operation::goals).goals;
    EXPECT_EQ(std::count_if(goals.begin(), goals.end(),
                            [](const auto &goal) {
                                return std::string(goal.goal_type_id, goal.goal_type_id_size) ==
                                           "assistant.reminder" &&
                                       goal.lifecycle == AYNI_GOAL_PROPOSED;
                            }),
              ayni::applications::assistant::model_goals_waiting_max);
    const auto proposed = [&] {
        std::vector<uint64_t> ids;
        for (const auto &notice : notices) {
            if (notice.kind == NoticeKind::goal_proposed) {
                ids.push_back(notice.id);
            }
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    };
    const auto announced = proposed();
    EXPECT_EQ(announced.size(), ayni::applications::assistant::model_goals_waiting_max);
    // Every Goal still waiting is told again after a restart, once.
    ASSERT_EQ(app.close(), AYNI_OK);
    notices.clear();
    ASSERT_EQ(app.open(directory.path(), config), AYNI_OK);
    settle();
    EXPECT_EQ(proposed(), announced);
    ASSERT_EQ(app.close(), AYNI_OK);
    unsetenv("AYNI_P7_FIXTURE_KEY");
}
// ADR-0072: a stop that lands after the schedule tool took a request but before it decided
// registers nothing; the tool reports the moot call as cancelled without effect.
TEST_F(NativeAssistant, stop_before_the_schedule_tool_decides_registers_nothing) {
    ASSERT_EQ(app.close(), AYNI_OK);
    const std::string answer =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"不应出现"}}]})";
    const std::string tool =
        R"({"usage":{"prompt_tokens":100,"completion_tokens":10,"total_tokens":110},"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"fixture","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"binding_id\":\"schedule\",\"binding_version\":1,\"arguments\":{\"kind\":\"remind\",\"local_time\":\"2030-01-02T09:00\",\"timezone\":\"UTC0\",\"text\":\"喝水\"}}"}}]}}]})";
    ayni::test::LoopbackHttp server(answer, 200, 0, {}, [&](const std::string &, unsigned count) {
        return count == 1 ? tool : answer;
    });
    ASSERT_EQ(setenv("AYNI_P7_FIXTURE_KEY", "local-fixture-only", 1), 0);
    ModelConfig config;
    config.model = Model::remote;
    config.harness.context_window_tokens = 5000;
    config.harness.max_output_tokens = 1000;
    config.harness.compact_target_tokens = 1000;
    config.harness.tool_result_max_tokens = 1000;
    config.base_url = server.url();
    config.model_name = "fixture";
    config.key_environment = "AYNI_P7_FIXTURE_KEY";
    ASSERT_EQ(app.open(directory.path(), config), AYNI_OK);
    settle();
    allow_chat();
    const auto turn = run(Operation::say, 0, "明早九点提醒我喝水");
    ASSERT_EQ(turn.status, AYNI_OK);
    // One pass settles the tool before it steps Core, so a call delivered to the tool by the
    // last pass is still undecided until the next one.
    uint64_t delivered = 0;
    const auto end = std::chrono::steady_clock::now() + async_test_timeout;
    while (!delivered && std::chrono::steady_clock::now() < end) {
        ASSERT_EQ(app.advance(notices), AYNI_OK);
        const auto calls = run(Operation::calls).calls;
        const bool model_answered = std::any_of(
            calls.begin(), calls.end(), [](const auto &call) { return call.tool_call_count; });
        for (const auto &call : calls) {
            if (model_answered && !call.outcome && call.wait_reason == AYNI_CALL_WAIT_PROVIDER) {
                delivered = call.logical_call_id;
            }
        }
        if (!delivered) {
            app.wait();
        }
    }
    ASSERT_NE(delivered, 0U);
    ASSERT_EQ(run(Operation::say, 0, "停").message, Message::stopped);
    settle();
    const auto calls = run(Operation::calls).calls;
    const auto call = std::find_if(calls.begin(), calls.end(),
                                   [&](const auto &c) { return c.logical_call_id == delivered; });
    ASSERT_NE(call, calls.end());
    EXPECT_EQ(call->outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(call->effect, AYNI_CALL_EFFECT_NONE);
    const auto goals = run(Operation::goals).goals;
    ASSERT_EQ(goals.size(), 1U);
    EXPECT_EQ(goals.front().goal_id, turn.id);
    EXPECT_EQ(goals.front().lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(server.requests(), 1U);
    EXPECT_EQ(std::count_if(
                  notices.begin(), notices.end(),
                  [](const Notice &notice) { return notice.kind == NoticeKind::goal_proposed; }),
              0);
    ASSERT_EQ(app.close(), AYNI_OK);
    unsetenv("AYNI_P7_FIXTURE_KEY");
}
TEST(SimDemo, independent_nodes_use_the_same_profile_and_retain_storage_on_release) {
    SimNode first, second;
    ayni::runtime::MemoryBodies bodies;
    for (uint64_t expected_epoch = 1; expected_epoch <= 2; ++expected_epoch) {
        for (auto *node : {&first, &second}) {
            DemoProfile profile;
            ASSERT_EQ(node->assemble(demo_config()), AYNI_OK);
            const ayni::providers::presentation::Output sink{
                nullptr, [](void *, ayni_string_v1) { return AYNI_OK; }};
            ASSERT_EQ(profile.assemble(node->kernel(), {},
                                       {node->clock(), sink, {}, {}, {}, bodies.store()}),
                      AYNI_OK);
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
