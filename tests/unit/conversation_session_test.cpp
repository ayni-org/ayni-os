// Shared interactive lifecycle: public APIs, real asynchronous Native files and built-in ToolLoop.
// ADR-0094 and the shared interactive runtime decision; no fault/recovery injection.
#include "temp_directory.h"
#include <ayni/calls.h>
#include <ayni/commands.hpp>
#include <ayni/hosts/native.h>
#include <ayni/runtime/portable.h>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <new>
namespace {
using ayni::initialized;
using ayni::runtime::conversation::Session;
using ayni::runtime::conversation::TurnResult;
using ayni::runtime::conversation::UnfinishedTurn;
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
class ConversationSession : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(host.open_storage(directory.path().c_str(),
                                    ayni::runtime::portable::host_request_capacity_min)
                      .status,
                  ayni::hosts::native::StorageStatus::ok);
        ASSERT_EQ(ayni::runtime::portable::Profile::memory_requirements(memory), AYNI_OK);
        arena = ::operator new(memory.bytes, std::align_val_t(memory.alignment), std::nothrow);
        ASSERT_NE(arena, nullptr);
        ASSERT_EQ(
            profile.create_in(arena, memory.bytes, host.table(), host.value_store(), persistence),
            AYNI_OK);
        ASSERT_EQ(profile.start(), AYNI_OK);
        ASSERT_TRUE(until([&] { return profile.running(); }));
        for (const auto *id : {"fixture.tools", "fixture.summary", "software.sum",
                               "software.events", "memory.access"}) {
            const ayni_grant_create_v1 grant{sizeof(grant),
                                             1,
                                             text(id),
                                             profile.instance(0),
                                             0,
                                             AYNI_GRANT_PERMANENT,
                                             0,
                                             0,
                                             {sizeof(ayni_scope_v1), 0, nullptr}};
            ASSERT_TRUE(until([&] { return !profile.command_pending(); }));
            ASSERT_EQ(profile.submit(AYNI_INTENT_GRANT_CREATE, &grant).status, AYNI_OK);
        }
        ASSERT_TRUE(until([&] { return !profile.command_pending(); }));
    }
    void TearDown() override {
        if (profile.kernel()) {
            EXPECT_EQ(profile.begin_shutdown(), AYNI_OK);
            EXPECT_TRUE(until([&] { return profile.stopped(); }));
            host.close_storage();
            EXPECT_EQ(profile.destroy(), AYNI_OK);
        }
        if (arena && !profile.kernel()) {
            ::operator delete(arena, std::align_val_t(memory.alignment));
        }
    }
    template <class Done> bool until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!done()) {
            const auto status = profile.tick();
            if (status != AYNI_OK && status != AYNI_BUSY && status != AYNI_NOT_READY) {
                ADD_FAILURE() << "tick=" << status;
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                // Native fsync latency is a Host property. Use the independent normal
                // suite's 30-second observation window and retain public state on timeout.
                auto info = initialized<ayni_kernel_info_v1>();
                const auto queried = ayni_get_kernel_info(profile.kernel(), &info);
                ADD_FAILURE() << "normal lifecycle deadline expired; query=" << queried
                              << " lifecycle=" << info.lifecycle << " durable=" << info.durable_seq
                              << " goal=" << profile.conversation(0)->latest_goal()
                              << " active=" << profile.conversation(0)->active()
                              << " held=" << profile.conversation(0)->held()
                              << " pending=" << profile.command_pending();
                return false;
            }
            host.wait_for_wake(std::chrono::milliseconds(1));
        }
        return true;
    }
    void open(UnfinishedTurn unfinished = UnfinishedTurn::discard) {
        ayni_status_t status = AYNI_BUSY;
        ASSERT_TRUE(until([&] {
            status = profile.open_conversations(unfinished);
            return status != AYNI_BUSY;
        }));
        ASSERT_EQ(status, AYNI_OK);
    }
    void reopen() {
        ASSERT_EQ(profile.begin_shutdown(), AYNI_OK);
        ASSERT_TRUE(until([&] { return profile.stopped(); }));
        host.close_storage();
        ASSERT_EQ(profile.destroy(), AYNI_OK);
        ASSERT_EQ(host.open_storage(directory.path().c_str(),
                                    ayni::runtime::portable::host_request_capacity_min)
                      .status,
                  ayni::hosts::native::StorageStatus::ok);
        ASSERT_EQ(
            profile.create_in(arena, memory.bytes, host.table(), host.value_store(), persistence),
            AYNI_OK);
        ASSERT_EQ(profile.start(), AYNI_OK);
        ASSERT_TRUE(until([&] { return profile.running(); }));
    }
    Session &session() { return *profile.conversation(0); }
    TurnResult result() {
        TurnResult out;
        EXPECT_TRUE(until([&] { return session().take_result(out); }));
        return out;
    }
    ayni::strategies::Persistence persistence = ayni::strategies::Persistence::durable;
    ayni::test::TempDirectory directory;
    ayni::runtime::portable::Memory memory;
    void *arena = nullptr;
    ayni::runtime::portable::Profile profile;
    ayni::hosts::native::Host host; // Joined before any placement storage is freed.
};
TEST_F(ConversationSession,
       two_turns_publish_through_the_core_anchor_and_forget_then_accept_input) {
    open();
    ASSERT_TRUE(session().ready());
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    const auto first = result();
    ASSERT_TRUE(first.published);
    EXPECT_NE(first.task, 0U);
    auto context = initialized<ayni_context_info_v1>();
    ASSERT_EQ(ayni_get_context(profile.kernel(), profile.session(), profile.instance(0), &context),
              AYNI_OK);
    EXPECT_EQ(context.pending_action, AYNI_CONTEXT_IDLE);
    EXPECT_EQ(context.last_turn_id, session().record()->last_round);
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    ASSERT_TRUE(result().published);
    ASSERT_EQ(session().forget(AYNI_CONTEXT_ERASE_ALL), AYNI_OK);
    ASSERT_TRUE(until([&] { return session().ready(); }));
    EXPECT_EQ(session().record()->round_count, 0U);
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    EXPECT_TRUE(result().published);
}
TEST_F(ConversationSession,
       cancel_during_input_discards_only_the_draft_without_requesting_a_model) {
    open();
    ASSERT_TRUE(session().ready());
    const auto before = profile.model_submissions();
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    EXPECT_EQ(session().forget(AYNI_CONTEXT_ERASE_ALL), AYNI_BUSY);
    ASSERT_EQ(session().cancel(), AYNI_OK);
    EXPECT_FALSE(result().published);
    EXPECT_EQ(profile.model_submissions(), before);
    EXPECT_EQ(session().record()->draft.size, 0U);
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    EXPECT_TRUE(result().published);
}
TEST_F(ConversationSession,
       cancel_an_accepted_goal_waits_for_durable_termination_then_allows_a_new_turn) {
    open();
    ASSERT_TRUE(session().ready());
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    ASSERT_TRUE(until([&] { return session().latest_goal() != 0; }));
    const auto original = session().latest_goal();
    ASSERT_EQ(session().cancel(), AYNI_OK);
    EXPECT_FALSE(result().published);
    auto goal = initialized<ayni_goal_info_v1>();
    ASSERT_EQ(ayni_get_goal(profile.kernel(), original, &goal), AYNI_OK);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_CANCELLED);
    auto info = initialized<ayni_kernel_info_v1>();
    ASSERT_EQ(ayni_get_kernel_info(profile.kernel(), &info), AYNI_OK);
    EXPECT_LE(goal.commit_seq, info.durable_seq);
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    EXPECT_TRUE(result().published);
}
TEST_F(ConversationSession, retain_mode_requires_explicit_abandon_before_accepting_another_input) {
    open(UnfinishedTurn::retain);
    ASSERT_TRUE(session().ready());
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    ASSERT_EQ(session().cancel(), AYNI_OK);
    EXPECT_FALSE(result().published);
    EXPECT_FALSE(session().ready());
    EXPECT_EQ(session().begin(text("sum")), AYNI_BUSY);
    ASSERT_EQ(session().abandon(), AYNI_OK);
    ASSERT_TRUE(until([&] { return session().ready(); }));
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    EXPECT_TRUE(result().published);
}
TEST_F(ConversationSession, cancellation_after_publication_does_not_discard_the_confirmed_answer) {
    open();
    ASSERT_TRUE(session().ready());
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    ASSERT_TRUE(until([&] {
        auto context = initialized<ayni_context_info_v1>();
        return ayni_get_context(profile.kernel(), profile.session(), profile.instance(0),
                                &context) == AYNI_OK &&
               context.pending_action == AYNI_CONTEXT_PUBLISH;
    }));
    ASSERT_EQ(session().cancel(), AYNI_OK);
    auto drained = AYNI_BUSY;
    ASSERT_TRUE(until([&] {
        drained = session().quiesce();
        return drained != AYNI_BUSY;
    }));
    ASSERT_EQ(drained, AYNI_OK);
    EXPECT_TRUE(result().published);
    EXPECT_EQ(session().record()->last_round, 1U);
}
// Normal clean close/reopen, not reset, power loss or recovery fault injection.
TEST_F(ConversationSession, normal_reopen_retains_the_answer_without_resubmitting_its_model) {
    open();
    ASSERT_TRUE(session().ready());
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    ASSERT_TRUE(result().published);
    const auto last = session().record()->last_round;
    reopen();
    open();
    ASSERT_TRUE(session().ready());
    EXPECT_EQ(session().record()->last_round, last);
    EXPECT_EQ(profile.model_submissions(), 0U);
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    EXPECT_TRUE(result().published);
}
TEST_F(ConversationSession, normal_close_after_owner_cancellation_discards_only_the_saved_draft) {
    open();
    ASSERT_TRUE(session().ready());
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    ASSERT_TRUE(until([&] { return session().latest_goal() != 0; }));
    ASSERT_TRUE(until([&] { return !profile.command_pending(); }));
    auto goal = initialized<ayni_goal_info_v1>();
    ASSERT_EQ(ayni_get_goal(profile.kernel(), session().latest_goal(), &goal), AYNI_OK);
    const ayni_goal_target_v1 target{sizeof(target), 0, goal.goal_id};
    ASSERT_EQ(profile.submit(AYNI_INTENT_GOAL_CANCEL, &target, goal.revision).status, AYNI_OK);
    // The owner explicitly cancels its interactive work before shutdown, as the ESP
    // composition does. Core shutdown alone does not promise to discard persistent Goals.
    reopen();
    open();
    ASSERT_TRUE(session().ready());
    ASSERT_NE(session().record(), nullptr);
    EXPECT_EQ(session().record()->draft.size, 0U);
    EXPECT_EQ(profile.model_submissions(), 0U);
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    EXPECT_TRUE(result().published);
}
class BackgroundSession : public ConversationSession {
  protected:
    BackgroundSession() { persistence = ayni::strategies::Persistence::turn_boundary; }
};
// ADR-0104: actual file bodies, Core shared VALUE results, and consecutive normal turns.
TEST_F(BackgroundSession, consecutive_turns_trim_then_reopen_and_delete_shared_history) {
    open();
    for (uint32_t turn = 0; turn < 2; ++turn) {
        ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
        ASSERT_TRUE(result().published);
        // White-box component check: ESP uses the same FileBodies trim before its next input.
        auto &bodies = *static_cast<ayni::runtime::FileBodies *>(profile.bodies()->context);
        ASSERT_TRUE(until([&] { return bodies.trim_cache() == AYNI_OK; }));
    }
    const auto old_history = session().record()->history;
    reopen();
    open();
    std::vector<ayni::strategies::Round> rounds;
    ASSERT_TRUE(until([&] {
        return ayni::strategies::Conversation::rounds(*profile.bodies(), *session().record(),
                                                      rounds) == AYNI_OK;
    }));
    ASSERT_EQ(rounds.size(), 2U);
    EXPECT_EQ(rounds.back().number, 2U);
    ASSERT_EQ(session().forget(AYNI_CONTEXT_ERASE_ALL), AYNI_OK);
    ASSERT_TRUE(until([&] { return session().ready(); }));
    std::string bytes(old_history.size, '\0');
    ayni_status_t status = AYNI_NOT_READY;
    ASSERT_TRUE(until([&] {
        return (status = profile.bodies()->read(profile.bodies()->context, &old_history,
                                                reinterpret_cast<uint8_t *>(bytes.data()),
                                                old_history.size)) != AYNI_NOT_READY;
    }));
    EXPECT_EQ(status, AYNI_NOT_FOUND);
}
// ADR-0104: normal shutdown may finish the saved publication without another model call.
TEST_F(BackgroundSession, normal_close_at_publication_reopens_completed_checkpoint) {
    open();
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    ASSERT_TRUE(until([&] {
        auto context = initialized<ayni_context_info_v1>();
        return ayni_get_context(profile.kernel(), profile.session(), profile.instance(0),
                                &context) == AYNI_OK &&
               context.pending_action == AYNI_CONTEXT_PUBLISH && context.usable;
    }));
    reopen();
    open();
    EXPECT_EQ(profile.model_submissions(), 0U);
    EXPECT_EQ(session().record()->last_round, 1U);
    std::vector<ayni::strategies::Round> rounds;
    ASSERT_TRUE(until([&] {
        return ayni::strategies::Conversation::rounds(*profile.bodies(), *session().record(),
                                                      rounds) == AYNI_OK;
    }));
    ASSERT_EQ(rounds.size(), 1U);
}
// ADR-0104: normal shutdown drains the accepted ledger result beyond Core STOPPED.
TEST_F(BackgroundSession, normal_close_after_shared_tool_terminal_preserves_ledger_body) {
    open();
    ASSERT_EQ(session().begin(text("sum")), AYNI_OK);
    std::vector<uint64_t> ids(ayni::runtime::portable::configuration().call_capacity);
    auto saved = initialized<ayni_call_info_v1>();
    ASSERT_TRUE(until([&] {
        uint32_t count = 0;
        if (ayni_list_objects(profile.kernel(), AYNI_OBJECT_CALL, ids.data(),
                              static_cast<uint32_t>(ids.size()), &count) != AYNI_OK) {
            return false;
        }
        for (uint32_t at = 0; at < count; ++at) {
            auto info = initialized<ayni_call_info_v1>();
            if (ayni_get_call(profile.kernel(), ids[at], &info) == AYNI_OK &&
                info.outcome == AYNI_CALL_COMPLETED && info.result_kind == AYNI_CALL_RESULT_VALUE &&
                info.body.id) {
                saved = info;
                return true;
            }
        }
        return false;
    }));
    reopen();
    auto restored = initialized<ayni_call_info_v1>();
    ASSERT_EQ(ayni_get_call(profile.kernel(), saved.logical_call_id, &restored), AYNI_OK);
    ASSERT_EQ(restored.outcome, AYNI_CALL_COMPLETED);
    ASSERT_EQ(restored.effect, AYNI_CALL_EFFECT_COMPLETE);
    ASSERT_EQ(restored.body.id, saved.body.id);
    std::string body(restored.body.size, '\0');
    auto read = AYNI_NOT_READY;
    ASSERT_TRUE(until([&] {
        read = profile.bodies()->read(profile.bodies()->context, &restored.body,
                                      reinterpret_cast<uint8_t *>(body.data()), restored.body.size);
        return read != AYNI_NOT_READY;
    }));
    ASSERT_EQ(read, AYNI_OK);
    EXPECT_EQ(ayni_body_verify(&restored.body, {reinterpret_cast<const uint8_t *>(body.data()),
                                                restored.body.size}),
              AYNI_OK);
    EXPECT_EQ(profile.model_submissions(), 0U); // Reopen reads facts; it does not re-execute work.
}
} // namespace
