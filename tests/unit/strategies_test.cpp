// ADR-0056 / C-POLICY-005..012: real registered policies, local protocol only.
#include "strategies_fixture.h"

namespace {
using namespace ayni::test;

TEST_F(StrategiesFixture, rules_execute_fixed_versioned_steps_in_order) {
    const auto task = start("routine");
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].text, "first");
    post(0, text("checked"));
    pump();
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[1].text, "second");
    EXPECT_NE(pending[0].operation, pending[1].operation);
    post(1, text("shown"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.task(task).step_count, 2U);
    EXPECT_EQ(work.task(task).model_calls_used, 0U);
}
TEST_F(StrategiesFixture, rules_stop_after_known_failure) {
    const auto task = start("routine");
    post(0, {}, AYNI_CALL_FAILED, AYNI_DENIED);
    pump();
    EXPECT_EQ(pending.size(), 1U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_FAILED);
}
TEST_F(StrategiesFixture, rules_unknown_result_blocks_the_second_step) {
    const auto task = start("routine");
    post(0, {}, AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN);
    pump();
    EXPECT_EQ(pending.size(), 1U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task).hold_status, AYNI_RESULT_UNKNOWN);
}
TEST_F(StrategiesFixture, two_tools_then_a_new_model_round_complete_the_conversation_turn) {
    const auto task = start();
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(pending[0].capability, "model");
    propose(0);
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[1].capability, "echo");
    post(1, text("first result"));
    pump();
    ASSERT_EQ(pending.size(), 3U);
    EXPECT_EQ(pending[2].capability, "echo");
    post(2, text("second result"));
    pump();
    ASSERT_EQ(pending.size(), 4U);
    EXPECT_EQ(pending[3].capability, "model");
    EXPECT_NE(pending[3].text.find("first result"), std::string::npos);
    EXPECT_NE(pending[3].text.find("second result"), std::string::npos);
    post(3, text("done"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.task(task).model_calls_used, 2U);
    finish(task);
    ASSERT_EQ(conversation.round_count, 1U);
    EXPECT_EQ(string_of(conversation.rounds[0].user.view()), "hello");
    EXPECT_EQ(string_of(conversation.rounds[0].answer.view()), "done");
}
TEST_F(StrategiesFixture, unknown_first_tool_blocks_later_tools_and_models) {
    const auto task = start();
    propose(0);
    ASSERT_EQ(pending.size(), 2U);
    post(1, {}, AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN);
    pump();
    EXPECT_EQ(pending.size(), 2U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task).hold_status, AYNI_RESULT_UNKNOWN);
    EXPECT_EQ(conversation.round_count, 0U);
}
TEST_F(StrategiesFixture, kernel_rejects_a_proposal_outside_the_goal_allow_list) {
    const auto task = start();
    propose(0, 1, "summary");
    EXPECT_EQ(pending.size(), 1U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task).hold_status, AYNI_RESULT_UNKNOWN);
}
TEST_F(StrategiesFixture, default_eight_model_calls_pause_before_the_ninth) {
    const auto task = start();
    for (uint32_t round = 0; round < AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT; ++round) {
        const uint32_t model = round * 2U;
        ASSERT_EQ(pending.size(), model + 1U);
        propose(model, 1);
        ASSERT_EQ(pending.size(), model + 2U);
        post(model + 1U, text("ok"));
        pump();
    }
    EXPECT_EQ(pending.size(), 16U);
    const auto info = work.task(task);
    EXPECT_EQ(info.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(info.model_calls_used, 8U);
    EXPECT_EQ(info.model_call_limit_reached, 1U);
}
TEST_F(StrategiesFixture, eight_tools_are_executed_sequentially) {
    const auto task = start();
    propose(0, AYNI_CALL_TOOL_COUNT_MAX);
    for (uint32_t at = 1; at <= AYNI_CALL_TOOL_COUNT_MAX; ++at) {
        ASSERT_EQ(pending.size(), at + 1U);
        post(at, text("ok"));
        pump();
    }
    ASSERT_EQ(pending.size(), 10U);
    post(9, text("done"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(StrategiesFixture, summary_replaces_old_rounds_only_in_a_successful_candidate) {
    for (uint32_t at = 0; at < policies::recent_rounds_max; ++at) {
        const auto task = start("chat", "past question");
        post(at, text("past answer"));
        pump();
        finish(task);
    }
    const auto task = start("chat", "current question");
    ASSERT_EQ(pending.size(), 4U);
    EXPECT_EQ(pending[3].capability, "summary");
    EXPECT_EQ(conversation.round_count, 3U);
    EXPECT_EQ(conversation.summary.size, 0U);
    post(3, text("tentative summary"));
    pump();
    ASSERT_EQ(pending.size(), 5U);
    EXPECT_EQ(pending[4].capability, "model");
    EXPECT_NE(pending[4].text.find("Unconfirmed model summary"), std::string::npos);
    EXPECT_NE(pending[4].text.find("turns 1..2"), std::string::npos);
    EXPECT_EQ(conversation.round_count,
              3U); // Even a successful summary has not committed externally.
    post(4, text("current answer"));
    pump();
    finish(task);
    EXPECT_EQ(work.task(task).model_calls_used, 2U);
    EXPECT_EQ(conversation.round_count, 2U);
    EXPECT_EQ(conversation.summary_first, 1U);
    EXPECT_EQ(conversation.summary_last, 2U);
    EXPECT_EQ(conversation.rounds[0].number, 3U);
    EXPECT_EQ(conversation.rounds[1].number, 4U);
}
TEST_F(StrategiesFixture, unsuccessful_summary_preserves_every_original_round) {
    for (uint32_t at = 0; at < policies::recent_rounds_max; ++at) {
        const auto task = start();
        post(at, text("past answer"));
        pump();
        finish(task);
    }
    const auto task = start();
    post(3, {}, AYNI_CALL_FAILED, AYNI_NOT_READY);
    pump();
    EXPECT_EQ(pending.size(), 4U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(conversation.round_count, 3U);
    EXPECT_EQ(conversation.summary.size, 0U);
    policies::Encoded state;
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                        policies::state_bytes_max, &state.size),
              AYNI_BUSY);
}
TEST_F(StrategiesFixture, deleting_the_whole_conversation_refuses_a_late_turn) {
    const auto task = start();
    policies::ConversationRecord erased;
    ASSERT_EQ(policies::Conversation::erase(conversation, erased), AYNI_OK);
    post(0, text("late answer"));
    pump();
    policies::Encoded state;
    ASSERT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                        policies::state_bytes_max, &state.size),
              AYNI_OK);
    policies::ConversationRecord candidate;
    EXPECT_EQ(policies::Conversation::finish(erased, state.view(), candidate),
              AYNI_REVISION_CONFLICT);
    EXPECT_EQ(erased.round_count, 0U);
    EXPECT_EQ(erased.summary.size, 0U);
}
TEST_F(StrategiesFixture, completed_checkpoint_export_has_a_durable_barrier_and_copy_bounds) {
    const auto task = start();
    policies::Encoded state;
    state.size = 77;
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                        policies::state_bytes_max, &state.size),
              AYNI_BUSY);
    EXPECT_EQ(state.size, 77U);
    post(0, text("answer"));
    // Normal async append: pause stepping as soon as completion is visible.
    for (uint32_t at = 0; at < 200 && work.task(task).lifecycle != AYNI_TASK_COMPLETED; ++at) {
        work.node().step();
        if (work.task(task).lifecycle != AYNI_TASK_COMPLETED) {
            work.node().sim().complete_storage();
        }
    }
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                        policies::state_bytes_max, &state.size),
              AYNI_BUSY);
    pump();
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, nullptr, 0, &state.size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_GT(state.size, 0U);
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                        policies::state_bytes_max, &state.size),
              AYNI_OK);
}
// Normal clean restart for the explicitly planned strategy replacement, not a
// power-failure/recovery campaign. A completed first step retains its identity.
TEST_F(StrategiesFixture, declared_compatible_replacement_continues_without_redoing_a_step) {
    const auto task = start("routine");
    const auto goal = work.task(task).goal_id;
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    post(0, text("first done"));
    pump();
    policies::Rules replacement;
    const ayni_checkpoint_reader_v1 reader{text("rules"), 1, 1};
    ASSERT_EQ(replacement.configure({text("rules.next"), 2, &reader, 1}, &rule, 1), AYNI_OK);
    rules_desc = replacement.describe();
    work.node().reboot();
    work.node().recover();
    work.reopen_session();
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal).status, AYNI_OK);
    pump();
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[1].text, "second");
    post(1, text("done"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    // replacement and its borrowed reader must outlive the Kernel.
    ASSERT_EQ(work.node().stop_and_destroy(), AYNI_OK);
}
TEST_F(StrategiesFixture, undeclared_checkpoint_writer_pauses_without_restarting_the_task) {
    const auto task = start("routine");
    const auto goal = work.task(task).goal_id;
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    post(0, text("first done"));
    pump();
    policies::Rules replacement;
    ASSERT_EQ(replacement.configure({text("rules.next"), 2, nullptr, 0}, &rule, 1), AYNI_OK);
    rules_desc = replacement.describe();
    work.node().reboot();
    work.node().recover();
    work.reopen_session();
    const auto info = work.task(task);
    EXPECT_EQ(info.hold_status, AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(info.hold_reason, AYNI_REASON_STRATEGY_CHANGED);
    EXPECT_EQ(info.step_count, 1U);
    EXPECT_EQ(pending.size(), 1U);
    ASSERT_EQ(work.node().stop_and_destroy(), AYNI_OK);
}
} // namespace

namespace {
// C-POLICY-007: registry input is copied, bounded and exact-self by default.
TEST(CheckpointReaders, registration_rejects_invalid_ranges_and_copies_accepted_readers) {
    Node node;
    node.config().strategy_capacity = 2;
    node.create();
    auto desc = strategy_desc(
        "new", 2,
        [](void *, const ayni_task_snapshot_v1 *, ayni_policy_output_v1 *) {
            return AYNI_UNSUPPORTED;
        },
        nullptr);
    ayni_checkpoint_reader_v1 readers[] = {{text("old"), 1, 3}, {text("other"), 1, 1}};
    desc.reader_count = 1;
    desc.readers = nullptr;
    EXPECT_EQ(ayni_register_strategy(node.kernel(), &desc), AYNI_INVALID_ARGUMENT);
    desc.readers = readers;
    readers[0].min_version = 0;
    EXPECT_EQ(ayni_register_strategy(node.kernel(), &desc), AYNI_INVALID_ARGUMENT);
    readers[0].min_version = 4;
    EXPECT_EQ(ayni_register_strategy(node.kernel(), &desc), AYNI_INVALID_ARGUMENT);
    readers[0].min_version = 1;
    readers[1].strategy_id = text("old");
    desc.reader_count = 2;
    EXPECT_EQ(ayni_register_strategy(node.kernel(), &desc), AYNI_INVALID_ARGUMENT);
    desc.reader_count = AYNI_CHECKPOINT_READER_COUNT_MAX + 1U;
    EXPECT_EQ(ayni_register_strategy(node.kernel(), &desc), AYNI_INVALID_ARGUMENT);
    desc.reader_count = 1;
    desc.reader_reserved = 1;
    EXPECT_EQ(ayni_register_strategy(node.kernel(), &desc), AYNI_INVALID_ARGUMENT);
    desc.reader_reserved = 0;
    ASSERT_EQ(ayni_register_strategy(node.kernel(), &desc), AYNI_OK);
    readers[0].max_version = 1;
    // White-box: inspect the registered promise only; integration covers restart.
    const auto &registry = node.kernel()->catalogs.strategies;
    ayni::catalog::CatalogId id;
    ASSERT_TRUE(id.assign(text("new")));
    uint32_t index = 0;
    ASSERT_TRUE(registry.find(id, index));
    const auto &entry = registry.at(index);
    EXPECT_TRUE(entry.accepts_checkpoint(text("new"), 2));
    EXPECT_TRUE(entry.accepts_checkpoint(text("old"), 3));
    EXPECT_FALSE(entry.accepts_checkpoint(text("old"), 4));
    EXPECT_FALSE(entry.accepts_checkpoint(text("missing"), 1));
}
} // namespace
