#include "strategy_workspace_fixture.h"
// ADR-0056 / C-POLICY-005..012: real registered policies, local protocol only.
#include "strategies_fixture.h"
#include <ayni/ayni.hpp>

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
    const auto rounds = rounds_of(conversation);
    ASSERT_EQ(rounds.size(), 1U);
    EXPECT_EQ(rounds[0].user, "hello");
    EXPECT_EQ(rounds[0].answer, "done");
}
// ADR-0095: one input larger than the window minus the answer room is refused before any
// model request; a long input that fits is sent as is.
TEST_F(StrategiesFixture, single_input_beyond_the_window_is_refused_without_a_model_call) {
    const std::string fits(2000, 'f'), oversized(8000, 'o');
    auto task = start("chat", fits.c_str());
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_NE(pending[0].text.find(fits), std::string::npos);
    post(0, text("done"));
    pump();
    finish(task);
    task = start("chat", oversized.c_str());
    EXPECT_EQ(pending.size(), 1U);
    const auto info = work.task(task);
    EXPECT_NE(info.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(info.hold_status, AYNI_CAPACITY_EXCEEDED);
}
// ADR-0095: an answer is bounded by max_output_tokens only, not by a byte limit.
TEST_F(StrategiesFixture, long_answer_completes_and_is_retained_intact) {
    const auto task = start();
    const std::string answer(3000, 'a');
    post(0, {answer.data(), static_cast<uint32_t>(answer.size())});
    pump();
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    finish(task);
    const auto rounds = rounds_of(conversation);
    ASSERT_EQ(rounds.size(), 1U);
    EXPECT_EQ(rounds[0].answer, answer);
}
TEST_F(StrategiesFixture, unknown_first_tool_blocks_later_tools_and_models) {
    const auto task = start();
    propose(0, 3);
    ASSERT_EQ(pending.size(), 2U);
    post(1, {}, AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN);
    pump();
    EXPECT_EQ(pending.size(), 2U);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task).hold_status, AYNI_RESULT_UNKNOWN);
    EXPECT_EQ(conversation.round_count, 0U);
}
TEST_F(StrategiesFixture, unlisted_binding_returns_matching_tool_error_without_business_call) {
    const auto task = start();
    propose(0, 1, "summary");
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[1].capability, "model");
    EXPECT_NE(pending[1].text.find("No business Call was accepted"), std::string::npos);
    post(1, text("refused"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(StrategiesFixture, default_eight_model_calls_pause_before_the_ninth) {
    const auto task = start();
    for (uint32_t round = 0; round < AYNI_MAX_TURNS_DEFAULT; ++round) {
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
        // ADR-0103: a whole model batch runs serially, without an intervening model request.
        EXPECT_EQ(pending[at].capability, "echo");
        post(at, text("ok"));
        pump();
    }
    ASSERT_EQ(pending.size(), 10U);
    EXPECT_EQ(pending.back().capability, "model");
    post(9, text("done"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.task(task).model_calls_used, 2U);
}
class BackgroundStrategies : public StrategiesFixture {
  protected:
    BackgroundStrategies() {
        configure_loop = [](policies::ToolLoopConfig &config) {
            config.persistence = policies::Persistence::background;
        };
    }
};
class BatchedForegroundTools : public BackgroundStrategies {
  protected:
    BatchedForegroundTools() { model_steps = &steps_seen; }
    std::vector<std::string> steps_seen;
};
// ADR-0103: one fixed reply advances the ordered foreground group under the original IDs.
TEST_F(BatchedForegroundTools, three_tools_keep_step_and_protocol_order_without_model_rounds) {
    const auto task = start();
    const auto model_call = work.task(task).logical_call_id;
    ASSERT_NE(model_call, 0U);
    const auto prefix = "fn." + std::to_string(model_call) + ".";
    propose(0, 3);
    for (uint32_t index = 0; index < 3; ++index) {
        const auto at = index + 1U;
        ASSERT_EQ(pending.size(), at + 1U);
        ASSERT_EQ(steps_seen.size(), at);
        EXPECT_EQ(pending[at].capability, "echo");
        EXPECT_EQ(steps_seen[index], prefix + std::to_string(index));
        post(at, text("result"));
        pump();
    }
    ASSERT_EQ(pending.size(), 5U);
    EXPECT_EQ(pending.back().capability, "model");
    const auto &prompt = pending.back().text;
    ayni::model::Reader reader({prompt.data(), static_cast<uint32_t>(prompt.size())});
    ayni::model::Message message;
    uint32_t index = 0;
    while (reader.next(message)) {
        if (message.role == ayni::model::Role::tool) {
            EXPECT_EQ(string_of(message.tool_id), "fixture_0_" + std::to_string(index++));
        }
    }
    EXPECT_TRUE(reader.finished());
    EXPECT_EQ(index, 3U);
    post(4, text("done"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.task(task).model_calls_used, 2U);
}
class ReusedMessageBodies : public BackgroundStrategies {
  protected:
    static inline thread_local std::map<std::string, uint32_t> staged;
    ayni_body_store_v1 observed{};
    void SetUp() override {
        observed = *bodies.store();
        observed.stage = [](void *ctx, uint64_t scope, ayni_bytes_v1 bytes, ayni_body_ref_v1 *out) {
            ++staged[std::string(reinterpret_cast<const char *>(bytes.data), bytes.size)];
            auto &memory = *static_cast<ayni::runtime::MemoryBodies *>(ctx);
            return memory.store()->stage(ctx, scope, bytes, out);
        };
        body_store = &observed;
        staged.clear();
        BackgroundStrategies::SetUp();
    }
};
// C-POLICY-013: user/assistant frames use their already owned raw body references.
TEST_F(ReusedMessageBodies, history_index_reuses_input_and_final_answer_bodies) {
    const auto task = start();
    ASSERT_EQ(pending.size(), 1U);
    EXPECT_EQ(staged["hello"], 0U); // begin admitted this input before the strategy ran.
    post(0, text("one final answer"));
    pump();
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(staged["one final answer"], 1U);
    finish(task);
    const auto history = text_of(conversation.history);
    EXPECT_NE(history.find("hello"), history.npos);
    EXPECT_NE(history.find("one final answer"), history.npos);
}
class CountedCompaction : public StrategiesFixture {
  protected:
    uint32_t replacement_projections = 0;
    CountedCompaction() {
        configure_loop = [this](policies::ToolLoopConfig &config) {
            config.projection.context = &replacement_projections;
            config.projection.context_writer = [](void *ctx, ayni::json::Writer &out,
                                                  ayni_string_v1 input, ayni::model::Mode mode,
                                                  ayni::model::Completeness complete) {
                if (mode == ayni::model::Mode::assistant &&
                    std::string_view(input.data, input.size).find("Unconfirmed model summary") !=
                        std::string_view::npos) {
                    ++*static_cast<uint32_t *>(ctx);
                }
                return ayni::model::write_context(out, input, mode, complete);
            };
            config.projection.reply_writer = [](void *, ayni::json::Writer &out, ayni_string_v1 id,
                                                ayni_string_v1 content) {
                out.raw(",{\"role\":\"tool\",\"tool_call_id\":");
                out.string(id);
                out.raw(",\"content\":");
                out.string(content);
                out.raw("}");
            };
        };
    }
};
// C-POLICY-012/013: compact replacement enters the normal request projection once.
TEST_F(CountedCompaction, completed_summary_is_projected_only_for_the_next_request) {
    const auto task = start_until_budget_summary();
    ASSERT_NE(task, 0U);
    uint32_t chunks = 0;
    while (pending.back().capability == "summary") {
        ASSERT_LT(chunks++, 32U);
        post(static_cast<uint32_t>(pending.size() - 1U), text("short summary"));
        pump();
    }
    ASSERT_EQ(pending.back().capability, "model");
    EXPECT_EQ(replacement_projections, 1U);
    post(static_cast<uint32_t>(pending.size() - 1U), text("done"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
class UnrelatedBodyWork : public StrategiesFixture {
  protected:
    uint32_t progress_calls = 0;
    UnrelatedBodyWork() {
        configure_loop = [this](policies::ToolLoopConfig &config) {
            config.body_progress = {&progress_calls, [](void *context, uint64_t) {
                                        ++*static_cast<uint32_t *>(context);
                                        return 0U; // Other body work keeps the worker busy.
                                    }};
        };
    }
};
// C-POLICY-013: durable mode waits for its own store operations, not queue-wide idle.
TEST_F(UnrelatedBodyWork, durable_task_completes_while_unrelated_body_work_remains) {
    const auto task = start();
    ASSERT_EQ(pending.size(), 1U);
    propose(0, 1);
    ASSERT_EQ(pending.size(), 2U);
    post(1, text("result"));
    pump();
    ASSERT_EQ(pending.size(), 3U);
    post(2, text("answer"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_GT(progress_calls, 0U);
    finish(task);
}
class ReusedHistory : public BackgroundStrategies {
  protected:
    static inline thread_local uint64_t watched_history = 0;
    static inline thread_local uint32_t history_reads = 0;
    ayni_body_store_v1 observed{};
    void SetUp() override {
        observed = *bodies.store();
        observed.read = [](void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                           uint32_t capacity) {
            history_reads += ref->id == watched_history ? 1U : 0U;
            auto &memory = *static_cast<ayni::runtime::MemoryBodies *>(context);
            return memory.store()->read(context, ref, out, capacity);
        };
        body_store = &observed;
        watched_history = history_reads = 0;
        BackgroundStrategies::SetUp();
    }
};
// C-POLICY-013: the next Task reuses the exact completed graph within the existing bound;
// the same owner privacy-progress input still clears all decoded private representations.
TEST_F(ReusedHistory, next_turn_reuses_completed_history_and_privacy_clears_it) {
    auto task = start();
    propose(0, 1);
    post(1, text("first tool result"));
    pump();
    post(2, text("first answer"));
    pump();
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    finish(task);
    auto privacy = ayni::initialized<ayni_privacy_info_v1>();
    ASSERT_EQ(ayni_get_privacy_status(work.kernel(), work.session(), &privacy), AYNI_OK);
    ASSERT_EQ(loop.maintain(work.kernel(), privacy), AYNI_OK);
    ASSERT_EQ(loop.cached_turns(), 1U);
    watched_history = conversation.history.id;
    history_reads = 0;
    task = start();
    ASSERT_EQ(pending.size(), 4U);
    EXPECT_NE(pending.back().text.find("first tool result"), std::string::npos);
    EXPECT_NE(pending.back().text.find("first answer"), std::string::npos);
    // Current ownership/digest remain checked; the cached graph needs no second
    // history projection. This permits the discovery and preparation reads only.
    EXPECT_GT(history_reads, 0U);
    EXPECT_LE(history_reads, 2U);
    EXPECT_EQ(loop.cached_turns(), 1U);
    post(3, text("second answer"));
    pump();
    finish(task);
    privacy.phase = AYNI_PRIVACY_ERASING;
    privacy.request_seq = 1;
    ASSERT_EQ(loop.maintain(work.kernel(), privacy), AYNI_OK);
    EXPECT_EQ(loop.cached_turns(), 0U);
}
// ADR-0104: shared staged bodies preserve ordering for the maximum serial proposal batch.
TEST_F(BackgroundStrategies, eight_tools_preserve_message_order_and_completed_round) {
    const auto task = start();
    propose(0, AYNI_CALL_TOOL_COUNT_MAX);
    for (uint32_t at = 1; at <= AYNI_CALL_TOOL_COUNT_MAX; ++at) {
        ASSERT_EQ(pending.size(), at + 1U);
        EXPECT_EQ(pending[at].capability, "echo");
        post(at, text("result"));
        pump();
    }
    ASSERT_EQ(pending.size(), 10U);
    EXPECT_EQ(pending.back().capability, "model");
    post(9, text("done"));
    pump();
    ASSERT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    finish(task);
    const auto rounds = rounds_of(conversation);
    ASSERT_EQ(rounds.size(), 1U);
    EXPECT_EQ(rounds[0].user, "hello");
    EXPECT_EQ(rounds[0].answer, "done");
    const auto messages = text_of(conversation.history);
    ayni::model::Reader reader({messages.data(), static_cast<uint32_t>(messages.size())});
    ayni::model::Message message;
    uint32_t results = 0;
    while (reader.next(message)) {
        results += message.role == ayni::model::Role::tool ? 1U : 0U;
    }
    EXPECT_TRUE(reader.finished());
    EXPECT_EQ(results, AYNI_CALL_TOOL_COUNT_MAX);
}
// ADR-0085: serving-model token observations trigger compression of older groups.
TEST_F(StrategiesFixture, summary_replaces_history_only_after_all_budget_chunks_succeed) {
    const auto task = start_until_budget_summary();
    ASSERT_NE(task, 0U);
    const auto old_round = conversation.last_round;
    const auto old_history = text_of(conversation.history);
    // ADR-0095: a summary is bounded by max_output_tokens only, not by a byte limit.
    const std::string summary_text(1500, 's');
    uint32_t summaries = 0;
    while (pending.back().capability == "summary") {
        ++summaries;
        ASSERT_LT(summaries, AYNI_MAX_TURNS_DEFAULT);
        EXPECT_EQ(conversation.summary.id, 0U);
        EXPECT_EQ(conversation.last_round, old_round);
        EXPECT_EQ(text_of(conversation.history), old_history);
        post(static_cast<uint32_t>(pending.size() - 1U),
             {summary_text.data(), static_cast<uint32_t>(summary_text.size())});
        pump();
    }
    EXPECT_GT(summaries, 0U);
    ASSERT_EQ(pending.back().capability, "model");
    EXPECT_NE(pending.back().text.find("Unconfirmed model summary"), std::string::npos);
    auto measured = ayni::json::Writer::measuring();
    const auto &body = pending.back().text;
    ASSERT_EQ(ayni::model::write_context(measured,
                                         {body.data(), static_cast<uint32_t>(body.size())},
                                         ayni::model::Mode::assistant),
              AYNI_OK);
    size_t cost = 0;
    ASSERT_TRUE(measured.finish(cost));
    auto baseline = ayni::json::Writer::measuring();
    ASSERT_EQ(ayni::model::write_context(
                  baseline, {old_history.data(), static_cast<uint32_t>(old_history.size())},
                  ayni::model::Mode::assistant),
              AYNI_OK);
    size_t previous_cost = 0;
    ASSERT_TRUE(baseline.finish(previous_cost));
    EXPECT_LT(cost,
              previous_cost); // Compare the same wire encoding, including fixed tool definitions.
    post(static_cast<uint32_t>(pending.size() - 1U), text("current answer"));
    pump();
    finish(task);
    EXPECT_EQ(conversation.last_round, old_round + 1U);
    EXPECT_GE(text_of(conversation.summary).size(), summary_text.size());
    // Retained user inputs are bounded by compact_target_tokens, newest first.
    const auto rounds = rounds_of(conversation);
    ASSERT_FALSE(rounds.empty());
    EXPECT_EQ(rounds.back().number, conversation.last_round);
    uint64_t retained = 0;
    for (const auto &round : rounds) {
        retained += ayni::model::estimate_tokens(round.user.size());
    }
    EXPECT_LE(retained, 1000U);
    EXPECT_LT(rounds.size(), conversation.last_round);
    EXPECT_EQ(conversation.summary_first, 1U);
    EXPECT_EQ(conversation.summary_last, old_round);
    EXPECT_GT(conversation.context_version, 1U);
}
TEST_F(StrategiesFixture, unsuccessful_summary_preserves_every_original_message) {
    const auto task = start_until_budget_summary();
    ASSERT_NE(task, 0U);
    const auto old = text_of(conversation.history);
    const auto old_round = conversation.last_round;
    post(static_cast<uint32_t>(pending.size() - 1U), {}, AYNI_CALL_FAILED, AYNI_NOT_READY);
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(conversation.last_round, old_round);
    EXPECT_EQ(conversation.summary.id, 0U);
    EXPECT_EQ(text_of(conversation.history), old);
}
TEST_F(StrategiesFixture, deleting_the_whole_conversation_refuses_a_late_turn) {
    const auto task = start();
    policies::ConversationRecord erased;
    ASSERT_EQ(
        policies::Conversation::erase(::ayni::test::conversation_workspace(), conversation, erased),
        AYNI_OK);
    post(0, text("late answer"));
    pump();
    policies::Encoded state;
    ASSERT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                        policies::encoded_bytes_max, &state.size),
              AYNI_OK);
    policies::ConversationRecord candidate;
    EXPECT_EQ(policies::Conversation::finish(::ayni::test::conversation_workspace(), erased,
                                             state.view(), candidate),
              AYNI_REVISION_CONFLICT);
    EXPECT_EQ(erased.round_count, 0U);
    EXPECT_EQ(erased.summary.id, 0U);
}
TEST_F(StrategiesFixture, completed_checkpoint_export_has_a_durable_barrier_and_copy_bounds) {
    const auto task = start();
    policies::Encoded state;
    state.size = 77;
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                        policies::encoded_bytes_max, &state.size),
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
                                        policies::encoded_bytes_max, &state.size),
              AYNI_BUSY);
    pump();
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, nullptr, 0, &state.size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_GT(state.size, 0U);
    EXPECT_EQ(ayni_copy_task_checkpoint(work.kernel(), task, state.bytes.data(),
                                        policies::encoded_bytes_max, &state.size),
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
    const auto &registry = node.kernel()->catalog().strategies;
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

// ADR-0084: nested native proposals pass through ToolLoop unchanged, and the
// next model round receives named structured results with exact numeric strings.
class StructuredToolLoop : public ayni::test::StrategiesFixture {
  protected:
    void SetUp() override {
        structured_tool = true;
        StrategiesFixture::SetUp();
    }
};
TEST_F(StructuredToolLoop, nested_input_and_structured_result_complete_the_same_conversation) {
    using namespace ayni::test;
    const auto task = start();
    ASSERT_EQ(pending.size(), 1U);
    ayni::ValueBuilder<4> input;
    input.object();
    input.list(7);
    input.object();
    input.decimal(3, {123, 2, 0});
    input.end();
    input.end();
    input.end();
    std::array<ayni_argument_node_v1, 4> arguments{};
    arguments[0].kind = AYNI_ARGUMENT_OBJECT;
    arguments[0].subtree_end = 4;
    arguments[1].kind = AYNI_ARGUMENT_LIST;
    arguments[1].subtree_end = 4;
    arguments[1].name = text("items");
    arguments[2].kind = AYNI_ARGUMENT_OBJECT;
    arguments[2].subtree_end = 4;
    arguments[3].kind = AYNI_ARGUMENT_STRING;
    arguments[3].subtree_end = 4;
    arguments[3].name = text("amount");
    arguments[3].value.text = text("1.23");
    FunctionProposal source(text("echo"),
                            {arguments.data(), static_cast<uint32_t>(arguments.size())},
                            text("nested_1"));
    auto proposal = source.call();
    post(0, {}, AYNI_CALL_COMPLETED, AYNI_OK, &proposal, 1);
    input.clear();
    pump();
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[1].text, R"({"items":[{"amount":"1.23"}]})");
    ayni::ValueBuilder<3> result;
    result.object();
    result.unsigned_integer(1, UINT64_MAX);
    result.decimal(2, {12325, 2, 0});
    result.end();
    ayni_provider_result_v1 receipt{};
    receipt.struct_size = sizeof(receipt);
    receipt.attempt = pending[1].attempt;
    receipt.outcome = AYNI_CALL_COMPLETED;
    receipt.effect = AYNI_CALL_EFFECT_COMPLETE;
    ASSERT_EQ(result.finish(receipt.value), AYNI_OK);
    ASSERT_EQ(ayni_provider_post(pending[1].completion, &receipt), AYNI_OK);
    pending[1].ended = true;
    result.clear();
    pump();
    ASSERT_EQ(pending.size(), 3U);
    EXPECT_NE(pending[2].text.find(R"({"items":[{"amount":"1.23"}]})"), std::string::npos);
    EXPECT_NE(pending[2].text.find(R"({"total":"18446744073709551615","price":"123.25"})"),
              std::string::npos);
    EXPECT_NE(pending[2].text.find("nested_1"), std::string::npos);
    post(2, text("done"));
    pump();
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    finish(task);
}

namespace {
// ADR-0085: model-visible pagination survives its own model/progress commits.
// ADR-0095: compaction starts once the estimated input exceeds the window minus the answer
// and one tool result, here 5000 - 1000 - 500 = 3500 tokens (a 75% rule would give 3000).
class CompactionTrigger : public ayni::test::StrategiesFixture {
    void SetUp() override {
        limits.tool_result_max_tokens = 500;
        StrategiesFixture::SetUp();
    }

  protected:
    // One turn on top of a reported prefix of input tokens; returns the first Call's capability.
    std::string next_turn(uint64_t reported) {
        conversation.last_usage = {AYNI_MODEL_USAGE_REPORTED, 0, reported, 10, reported + 10};
        const auto task = start("chat", "next");
        EXPECT_FALSE(pending.empty());
        const auto capability = pending.empty() ? std::string{} : pending.back().capability;
        if (capability == "model") {
            post(static_cast<uint32_t>(pending.size() - 1U), text("answer"));
            pump();
            finish(task);
        }
        return capability;
    }
};
TEST_F(CompactionTrigger, compaction_starts_past_window_minus_output_minus_tool_result) {
    ASSERT_EQ(next_turn(100), "model"); // Gives the next turns an older group to compact.
    EXPECT_EQ(next_turn(3200), "model");
    EXPECT_EQ(next_turn(3500), "summary");
}
class ManyCapabilities : public ayni::test::StrategiesFixture {
    void SetUp() override {
        tool_alias_count = 24;
        limits.context_window_tokens = 16000; // One listing page of 16 capabilities fits.
        limits.tool_result_max_tokens = 8000;
        StrategiesFixture::SetUp();
    }
};
TEST_F(ManyCapabilities, native_list_inspect_and_generic_call_bind_only_at_execution) {
    using namespace ayni::test;
    const auto task = start();
    const auto propose_function = [&](uint32_t at, const char *name, ayni_arguments_v1 arguments,
                                      const char *id) {
        ayni_tool_call_v1 call{};
        call.struct_size = sizeof(call);
        call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
        call.function_name = text(name);
        call.protocol_id = text(id);
        call.input.arguments = arguments;
        post(at, {}, AYNI_CALL_COMPLETED, AYNI_OK, &call, 1);
        pump();
    };
    const auto latest_tool = [&]() {
        const auto &body = pending.back().text;
        ayni::model::Reader reader({body.data(), static_cast<uint32_t>(body.size())});
        ayni::model::Message message;
        std::string answer;
        while (reader.next(message)) {
            if (message.role == ayni::model::Role::tool) {
                answer = string_of(message.text);
            }
        }
        EXPECT_TRUE(reader.finished());
        return answer;
    };
    ayni_argument_node_v1 empty{};
    empty.kind = AYNI_ARGUMENT_OBJECT;
    empty.subtree_end = 1;
    propose_function(0, "ayni_list_capabilities", {&empty, 1}, "list_original");
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending.back().capability, "model");
    const auto first = latest_tool();
    EXPECT_NE(first.find("tool.15"), std::string::npos);
    EXPECT_EQ(first.find("tool.16"), std::string::npos);
    const std::string marker = "\"next_cursor\":\"";
    const auto start = first.find(marker);
    ASSERT_NE(start, std::string::npos);
    const auto begin = start + marker.size();
    const auto end = first.find('"', begin);
    ASSERT_NE(end, std::string::npos);
    const auto cursor = first.substr(begin, end - begin);
    std::array<ayni_argument_node_v1, 2> next{};
    next[0].kind = AYNI_ARGUMENT_OBJECT;
    next[0].subtree_end = 2;
    next[1].kind = AYNI_ARGUMENT_STRING;
    next[1].subtree_end = 2;
    next[1].name = text("cursor");
    next[1].value.text = {cursor.data(), static_cast<uint32_t>(cursor.size())};
    propose_function(1, "ayni_list_capabilities", {next.data(), 2}, "list_next");
    ASSERT_EQ(pending.size(), 3U);
    const auto second = latest_tool();
    EXPECT_NE(second.find("tool.16"), std::string::npos);
    EXPECT_NE(second.find("tool.23"), std::string::npos);
    EXPECT_NE(second.find("\"next_cursor\":null"), std::string::npos);
    std::array<ayni_argument_node_v1, 3> selected{};
    selected[0].kind = AYNI_ARGUMENT_OBJECT;
    selected[0].subtree_end = 3;
    selected[1].kind = AYNI_ARGUMENT_STRING;
    selected[1].subtree_end = 2;
    selected[1].name = text("binding_id");
    selected[1].value.text = text("tool.23");
    selected[2].kind = AYNI_ARGUMENT_NUMBER;
    selected[2].subtree_end = 3;
    selected[2].name = text("binding_version");
    selected[2].value.text = text("1");
    propose_function(2, "ayni_inspect_capability", {selected.data(), 3}, "inspect_original");
    ASSERT_EQ(pending.size(), 4U);
    EXPECT_NE(latest_tool().find("\"input_schema\""), std::string::npos);
    EXPECT_NE(latest_tool().find("Echo input."), std::string::npos);
    ayni_task_tool_authority_v1 authority{};
    authority.struct_size = sizeof(authority);
    EXPECT_EQ(ayni_get_task_tool_authority(work.kernel(), task, 0, &authority), AYNI_NOT_FOUND);
    ayni_argument_node_v1 argument{};
    argument.kind = AYNI_ARGUMENT_STRING;
    argument.subtree_end = 1;
    argument.value.text = text("native result");
    FunctionProposal invocation(text("tool.23"), {&argument, 1}, text("invoke_original"));
    post(3, {}, AYNI_CALL_COMPLETED, AYNI_OK, &invocation.call(), 1);
    pump();
    ASSERT_EQ(pending.size(), 5U);
    EXPECT_EQ(pending.back().capability, "echo");
    ASSERT_EQ(ayni_get_task_tool_authority(work.kernel(), task, 0, &authority), AYNI_OK);
    EXPECT_NE(authority.original_grant_id, 0U);
    post(4, text("native result"));
    pump();
    ASSERT_EQ(pending.size(), 6U);
    EXPECT_EQ(pending.back().capability, "model");
    EXPECT_NE(pending.back().text.find("invoke_original"), std::string::npos);
    post(5, text("done"));
    pump();
    finish(task);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
// ADR-0085: the model must first consume the latest complete tool round.
TEST_F(StrategiesFixture, latest_completed_tool_group_is_preserved_for_the_next_model) {
    const auto task = start();
    propose(0, 2);
    const std::string result(1024, 'x');
    post(1, {result.data(), static_cast<uint32_t>(result.size())});
    pump();
    ASSERT_EQ(pending.size(), 3U);
    post(2, {result.data(), static_cast<uint32_t>(result.size())});
    pump();
    ASSERT_EQ(pending.back().capability, "model");
    EXPECT_NE(pending.back().text.find(result), std::string::npos);
    auto measured = ayni::json::Writer::measuring();
    const auto &frame = pending.back().text;
    ASSERT_EQ(ayni::model::write_context(measured,
                                         {frame.data(), static_cast<uint32_t>(frame.size())},
                                         ayni::model::Mode::assistant),
              AYNI_OK);
    post(static_cast<uint32_t>(pending.size() - 1U), text("done"));
    pump();
    finish(task);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
}
} // namespace
