// White-box normal call/Task transitions and record encoding, ADR-0045.
// No restart or fault-injection experiment is asserted by these scenarios.
#include "work/task/task_records.h"

#include <gtest/gtest.h>
#include <vector>

namespace {
using namespace ayni::work;
class TaskCallTest : public ::testing::Test {
  protected:
    void SetUp() override {
        ayni::foundation::IdAllocator ids;
        ids.start(1);
        ASSERT_TRUE(ids.allocate(call_id));
        ASSERT_TRUE(tasks.insert({1, 2, 0, 0, 1, 1}, 3, next()));
    }
    ayni::foundation::TransitionStamp next() {
        ++sequence;
        return {sequence, sequence};
    }
    void begin(TaskTable::CallKind kind = TaskTable::CallKind::tool) {
        tasks.begin_call(0, call_id, kind, next());
    }
    void change(TaskChange kind) {
        ASSERT_EQ(tasks.check(0, kind, tasks.at(0).revision), TaskTable::ChangeCheck::allowed);
        tasks.apply(0, kind, next());
    }
    TaskState saved_state() const {
        std::vector<uint8_t> body(task_state_record_size);
        write_task_state_record(tasks.at(0), body.data(), ayni::work::TaskRecordChange::state);
        TaskStateRecord decoded{};
        EXPECT_TRUE(
            read_task_state_record(body.data(), static_cast<uint32_t>(body.size()), decoded));
        return decoded.state;
    }
    std::vector<uint64_t> storage =
        std::vector<uint64_t>(TaskTable::storage_size({1, 4, 16}) / sizeof(uint64_t) + 1);
    TaskTable tasks{storage.data(), {1, 4, 16}};
    uint64_t sequence = 0;
    uint64_t call_id = 0;
};

// White-box, C-RUN-002 / ADR-0045: restoring the owner's wait-resume command
// changes only its wait overlay. Invalid partners cannot rewrite the call/count.
TEST_F(TaskCallTest, restored_resource_wait_resume_preserves_every_unrelated_task_fact) {
    begin(TaskTable::CallKind::model);
    tasks.pause_resource_wait(0, next());
    const auto before = tasks.at(0);
    const auto valid = saved_state();
    EXPECT_EQ(tasks.check_resume_wait(0, before.revision + 1), AYNI_REVISION_CONFLICT);
    struct Case {
        const char *name;
        void (*change)(TaskState &);
    };
    const Case cases[] = {
        {"wrong revision", [](auto &s) { ++s.revision; }},
        {"different lifecycle", [](auto &s) { s.lifecycle = AYNI_TASK_PAUSED; }},
        {"different cancellation", [](auto &s) { ++s.cancel_revision; }},
        {"hold retained", [](auto &s) { s.hold_status = AYNI_BUSY; }},
        {"hold reason retained", [](auto &s) { s.hold_reason = AYNI_REASON_RESOURCE_WAIT; }},
        {"different steps", [](auto &s) { ++s.step_count; }},
        {"different model count", [](auto &s) { ++s.model_calls.used; }},
        {"different model limit", [](auto &s) { ++s.model_calls.limit; }},
        {"different call", [](auto &s) { ++s.call.id; }},
    };
    for (const auto &test : cases) {
        SCOPED_TRACE(test.name);
        auto candidate = valid;
        ++candidate.revision;
        candidate.hold_status = AYNI_OK;
        candidate.hold_reason = AYNI_REASON_NONE;
        test.change(candidate);
        EXPECT_EQ(tasks.restore_wait_resume(0, candidate, next()), TaskTable::Restore::corrupt);
        EXPECT_EQ(tasks.at(0).revision, before.revision);
        EXPECT_EQ(tasks.at(0).hold_status, before.hold_status);
        EXPECT_EQ(tasks.at(0).call, before.call);
        EXPECT_EQ(tasks.at(0).model_calls, before.model_calls);
    }
    auto candidate = valid;
    ++candidate.revision;
    candidate.hold_status = AYNI_OK;
    candidate.hold_reason = AYNI_REASON_NONE;
    EXPECT_EQ(tasks.restore_wait_resume(0, candidate, {before.commit_seq, before.commit_seq}),
              TaskTable::Restore::corrupt);
    ASSERT_EQ(tasks.restore_wait_resume(0, candidate, next()), TaskTable::Restore::restored);
    EXPECT_EQ(tasks.at(0).hold_status, AYNI_OK);
    EXPECT_EQ(tasks.at(0).call, before.call);
    EXPECT_EQ(tasks.at(0).model_calls, before.model_calls);
    EXPECT_EQ(tasks.restore_wait_resume(0, candidate, next()), TaskTable::Restore::corrupt);
}

// White-box, ADR-0043: an exact model-limit raise may not change lifecycle,
// progress, the selected call or the number of accepted model requests.
TEST_F(TaskCallTest, restored_model_limit_raise_rejects_unrelated_changes_without_installing) {
    begin(TaskTable::CallKind::model);
    const auto before = tasks.at(0);
    auto valid = saved_state();
    ++valid.revision;
    ++valid.model_calls.limit;
    struct Case {
        const char *name;
        void (*change)(TaskState &);
    };
    const Case cases[] = {
        {"invalid model limit", [](auto &s) { s.model_calls.limit = 0; }},
        {"raise still blocked",
         [](auto &s) {
             s.model_calls.used = s.model_calls.limit;
             s.model_calls.blocked = true;
         }},
        {"count changed", [](auto &s) { ++s.model_calls.used; }},
        {"revision changed twice", [](auto &s) { ++s.revision; }},
        {"lifecycle changed", [](auto &s) { s.lifecycle = AYNI_TASK_PAUSED; }},
        {"cancel revision changed", [](auto &s) { ++s.cancel_revision; }},
        {"hold status changed", [](auto &s) { s.hold_status = AYNI_BUSY; }},
        {"hold reason changed", [](auto &s) { s.hold_reason = AYNI_REASON_RESOURCE_WAIT; }},
        {"steps changed", [](auto &s) { ++s.step_count; }},
        {"limit not raised", [](auto &s) { --s.model_calls.limit; }},
        {"call changed", [](auto &s) { ++s.call.id; }},
    };
    for (const auto &test : cases) {
        SCOPED_TRACE(test.name);
        auto candidate = valid;
        test.change(candidate);
        EXPECT_EQ(tasks.restore_model_limit_raise(0, candidate, next()),
                  TaskTable::Restore::corrupt);
        EXPECT_EQ(tasks.at(0).revision, before.revision);
        EXPECT_EQ(tasks.at(0).model_calls, before.model_calls);
        EXPECT_EQ(tasks.at(0).call, before.call);
    }
    EXPECT_EQ(tasks.restore_model_limit_raise(0, valid, {before.commit_seq, before.commit_seq}),
              TaskTable::Restore::corrupt);
    ASSERT_EQ(tasks.restore_model_limit_raise(0, valid, next()), TaskTable::Restore::restored);
    EXPECT_EQ(tasks.at(0).revision, before.revision + 1);
    EXPECT_EQ(tasks.at(0).model_calls.used, before.model_calls.used);
    EXPECT_EQ(tasks.at(0).model_calls.limit, valid.model_calls.limit);
}

TEST_F(TaskCallTest, model_acceptance_counts_once_and_the_result_reopens_decisions) {
    begin(TaskTable::CallKind::model);
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_RUNNING);
    EXPECT_EQ(tasks.at(0).call.id, call_id);
    EXPECT_EQ(tasks.at(0).model_calls.used, 1U);
    EXPECT_FALSE(tasks.has_ready());
    tasks.finish_call(0, next());
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(tasks.at(0).call.id, 0U);
    EXPECT_EQ(tasks.at(0).model_calls.used, 1U);
    EXPECT_TRUE(tasks.has_ready());
}

TEST_F(TaskCallTest, cancellation_pins_the_task_until_the_selected_call_is_drained) {
    begin();
    change(TaskChange::cancel);
    const auto cancel_revision = tasks.at(0).cancel_revision;
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_CANCELLING);
    EXPECT_EQ(tasks.at(0).call.id, call_id);
    EXPECT_FALSE(tasks.has_room());
    tasks.finish_call(0, next());
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(tasks.at(0).cancel_revision, cancel_revision);
    EXPECT_FALSE(tasks.has_ready());
    EXPECT_TRUE(tasks.has_room());
}

TEST_F(TaskCallTest, expiry_preserves_its_requested_end_while_waiting_for_a_terminal_result) {
    begin();
    change(TaskChange::expire);
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_CANCELLING);
    EXPECT_EQ(tasks.at(0).call.end, TaskCall::End::expire);
    tasks.finish_call(0, next());
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_EXPIRED);
    EXPECT_EQ(tasks.at(0).cancel_revision, 0U);
}

TEST_F(TaskCallTest, a_result_cannot_remove_the_users_pause) {
    begin();
    change(TaskChange::pause);
    tasks.finish_call(0, next());
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(tasks.at(0).call.id, 0U);
    EXPECT_FALSE(tasks.has_ready());
    change(TaskChange::resume);
    EXPECT_TRUE(tasks.has_ready());
}

TEST_F(TaskCallTest, unknown_call_never_reopens_decisions_after_task_cancellation) {
    begin();
    tasks.hold_unknown_call(0, next());
    EXPECT_EQ(TaskTable::effective_lifecycle(tasks.at(0)), AYNI_TASK_PAUSED);
    EXPECT_EQ(TaskTable::effective_hold_status(tasks.at(0)), AYNI_RESULT_UNKNOWN);
    EXPECT_TRUE(tasks.has_held());
    EXPECT_FALSE(TaskTable::allows(tasks.at(0), TaskChange::resume));
    change(TaskChange::cancel);
    EXPECT_EQ(TaskTable::effective_lifecycle(tasks.at(0)), AYNI_TASK_CANCELLING);
    EXPECT_FALSE(tasks.has_room());
    EXPECT_FALSE(tasks.has_held());
    // Provider has returned its borrow; the Call owner independently retains UNKNOWN.
    tasks.finish_call(0, next());
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_FALSE(tasks.has_ready());
}

TEST_F(TaskCallTest, reconciliation_preserves_a_pause_requested_while_the_result_was_unknown) {
    begin();
    tasks.hold_unknown_call(0, next());
    change(TaskChange::pause);
    tasks.finish_call(0, next());
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_FALSE(tasks.has_held());
    EXPECT_FALSE(tasks.has_ready());
    change(TaskChange::resume);
    EXPECT_TRUE(tasks.has_ready());
}

TEST_F(TaskCallTest, selected_call_records_keep_each_existing_pairing_meaning) {
    begin(TaskTable::CallKind::model);
    const TaskRecordChange changes[]{TaskRecordChange::state, TaskRecordChange::authority,
                                     TaskRecordChange::reauthorization,
                                     TaskRecordChange::raise_limit, TaskRecordChange::resume_wait};
    for (const auto change_kind : changes) {
        std::vector<uint8_t> body(task_state_record_size);
        write_task_state_record(tasks.at(0), body.data(), change_kind);
        TaskStateRecord decoded{};
        ASSERT_TRUE(
            read_task_state_record(body.data(), static_cast<uint32_t>(body.size()), decoded));
        EXPECT_EQ(decoded.change, change_kind);
        EXPECT_EQ(decoded.state.call.id, call_id);
        EXPECT_EQ(decoded.state.model_calls.used, 1U);
        EXPECT_EQ(decoded.state.lifecycle, AYNI_TASK_RUNNING);
    }
}
// C-COMMIT-012: reserve the UNKNOWN/cancel/drain path before accepting a call.
TEST_F(TaskCallTest, public_state_changes_preserve_the_last_revisions_needed_to_drain) {
    auto &task = const_cast<Task &>(tasks.at(0));
    task.revision = UINT32_MAX - 4;
    ASSERT_TRUE(TaskTable::can_begin_call(task));
    begin();
    EXPECT_EQ(tasks.check(0, TaskChange::pause, task.revision),
              TaskTable::ChangeCheck::revision_exhausted);
    tasks.hold_unknown_call(0, next());
    change(TaskChange::cancel);
    tasks.finish_call(0, next());
    EXPECT_EQ(tasks.at(0).revision, UINT32_MAX);
    EXPECT_EQ(tasks.at(0).lifecycle, AYNI_TASK_CANCELLED);
}
// ADR-0045: repeat wait/resume consumes revisions, never accepted model calls.
TEST_F(TaskCallTest, repeated_resource_wait_reserves_cancellation_and_drain_revisions) {
    begin(TaskTable::CallKind::model);
    tasks.pause_resource_wait(0, next());
    const auto revision = tasks.at(0).revision;
    EXPECT_EQ(tasks.check_resume_wait(0, revision), AYNI_OK);
    tasks.resume_wait(0, next());
    tasks.pause_resource_wait(0, next());
    EXPECT_EQ(tasks.at(0).model_calls.used, 1U);
    EXPECT_EQ(tasks.at(0).call.id, call_id);
    auto &task = const_cast<Task &>(tasks.at(0));
    task.revision = UINT32_MAX - 3;
    EXPECT_EQ(tasks.check_resume_wait(0, task.revision), AYNI_CAPACITY_EXCEEDED);
    change(TaskChange::cancel);
    tasks.finish_call(0, next());
    EXPECT_EQ(task.lifecycle, AYNI_TASK_CANCELLED);
}
} // namespace
