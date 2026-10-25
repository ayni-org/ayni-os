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
    std::vector<uint64_t> storage =
        std::vector<uint64_t>(TaskTable::storage_size({1, 4, 16}) / sizeof(uint64_t) + 1);
    TaskTable tasks{storage.data(), {1, 4, 16}};
    uint64_t sequence = 0;
    uint64_t call_id = 0;
};

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
    const uint16_t bases[]{task_state_record_version, task_state_authority_record_version,
                           task_state_reauthorization_record_version,
                           task_state_raise_limit_record_version,
                           task_state_resume_wait_record_version};
    for (const auto base : bases) {
        const auto version = task_state_version_for(tasks.at(0), base);
        EXPECT_GE(version, task_state_call_record_version);
        EXPECT_EQ(task_state_base_version(version), base);
        std::vector<uint8_t> body(task_state_size_for(version));
        write_task_state_record(tasks.at(0), body.data(), version);
        TaskStateRecord decoded{};
        ASSERT_TRUE(read_task_state_record(body.data(), static_cast<uint32_t>(body.size()), decoded,
                                           version));
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
