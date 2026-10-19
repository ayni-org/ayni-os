// Tasks owned by work/task (ADR-0018): revisions and the cancel revision
// (C-COMMIT-012), the transitions this part reaches, and the places ended
// tasks give up when the table is full.

#include "work/task/task_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using ayni::work::Task;
using ayni::work::TaskChange;
using ayni::work::TaskTable;
using ChangeCheck = TaskTable::ChangeCheck;

constexpr uint64_t owner = 50;
constexpr uint64_t goal_id = 70;

class Tasks {
  public:
    explicit Tasks(uint32_t capacity)
        : storage_(TaskTable::storage_size(capacity) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), capacity) {}

    TaskTable *operator->() { return &table_; }

    // Inserts the next task; returns its id, or 0 if refused.
    uint64_t create(uint32_t occurrence = 1) {
        const uint64_t task_id = last_id_ + 1U;
        if (!table_.insert({goal_id, owner, 900, 3, occurrence, 2}, task_id, next_stamp())) {
            return 0;
        }
        last_id_ = task_id;
        return task_id;
    }

    bool exists(uint64_t task_id) const {
        uint32_t index = 0;
        return table_.find(task_id, index);
    }

    const Task &task(uint64_t task_id) { return table_.at(index_of(task_id)); }

    uint32_t index_of(uint64_t task_id) {
        uint32_t index = 0;
        EXPECT_TRUE(table_.find(task_id, index));
        return index;
    }

    // Checks and applies; returns what check() said.
    ChangeCheck change(uint64_t task_id, TaskChange kind) {
        const uint32_t index = index_of(task_id);
        const ChangeCheck check = table_.check(index, kind, table_.at(index).revision);
        if (check == ChangeCheck::allowed) {
            table_.apply(index, kind, next_stamp());
        }
        return check;
    }

    // White-box: the tasks lie at the start of the storage.
    Task &raw(uint64_t task_id) {
        return reinterpret_cast<Task *>(storage_.data())[index_of(task_id)];
    }

    uint64_t last_seq() const { return last_seq_; }

  private:
    ayni::foundation::TransitionStamp next_stamp() {
        ++last_seq_;
        return {last_seq_ + 10U, last_seq_};
    }

    std::vector<uint64_t> storage_;
    TaskTable table_;
    uint64_t last_id_ = 0;
    uint64_t last_seq_ = 0;
};

TEST(TaskTable, new_task_is_ready_at_revision_1) {
    Tasks tasks(2);
    const uint64_t id = tasks.create(4);
    ASSERT_NE(id, 0U);
    const Task &task = tasks.task(id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(task.revision, 1U);
    EXPECT_EQ(task.cancel_revision, 0U);
    EXPECT_EQ(task.goal_id, goal_id);
    EXPECT_EQ(task.owner_instance_id, owner);
    EXPECT_EQ(task.goal_index, 3U);
    EXPECT_EQ(task.occurrence, 4U);
    EXPECT_EQ(task.occurrence_count, 2U);
    EXPECT_EQ(task.due_us, 900U);
    EXPECT_EQ(task.created_transition_id, 11U);
    EXPECT_EQ(task.commit_seq, 1U);
    EXPECT_FALSE(tasks.exists(id + 1U));
}

TEST(TaskTable, pause_resume_and_cancel_follow_the_task_lifecycle) {
    Tasks tasks(2);
    const uint64_t id = tasks.create();
    EXPECT_EQ(tasks.change(id, TaskChange::resume), ChangeCheck::invalid_state);
    ASSERT_EQ(tasks.change(id, TaskChange::pause), ChangeCheck::allowed);
    EXPECT_EQ(tasks.task(id).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(tasks.task(id).revision, 2U);
    EXPECT_EQ(tasks.task(id).commit_seq, tasks.last_seq());
    EXPECT_EQ(tasks.change(id, TaskChange::pause), ChangeCheck::invalid_state);
    ASSERT_EQ(tasks.change(id, TaskChange::resume), ChangeCheck::allowed);
    EXPECT_EQ(tasks.task(id).lifecycle, AYNI_TASK_READY);

    // A PAUSED task is cancelled too, and CANCELLED has no exit.
    ASSERT_EQ(tasks.change(id, TaskChange::pause), ChangeCheck::allowed);
    ASSERT_EQ(tasks.change(id, TaskChange::cancel), ChangeCheck::allowed);
    EXPECT_EQ(tasks.task(id).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(tasks.task(id).revision, 5U);
    EXPECT_EQ(tasks.task(id).cancel_revision, 5U);
    for (const TaskChange kind : {TaskChange::pause, TaskChange::resume, TaskChange::cancel}) {
        EXPECT_EQ(tasks.change(id, kind), ChangeCheck::invalid_state);
    }
    EXPECT_EQ(tasks.task(id).revision, 5U);
}

// C-COMMIT-012: a stale revision is a conflict before the state is checked,
// and a revision never wraps.
TEST(TaskTable, revision_is_checked_before_the_state_and_never_wraps) {
    Tasks tasks(2);
    const uint64_t id = tasks.create();
    const uint32_t index = tasks.index_of(id);
    EXPECT_EQ(tasks->check(index, TaskChange::resume, 2), ChangeCheck::revision_conflict);
    EXPECT_EQ(tasks->check(index, TaskChange::resume, 1), ChangeCheck::invalid_state);
    tasks.raw(id).revision = std::numeric_limits<uint32_t>::max();
    EXPECT_TRUE(TaskTable::at_largest_revision(tasks.task(id)));
    EXPECT_EQ(tasks.change(id, TaskChange::cancel), ChangeCheck::revision_exhausted);
    EXPECT_EQ(tasks.task(id).lifecycle, AYNI_TASK_READY);
}

// ADR-0018: a full table gives the place of the task that ended first to the
// new one; a task that has not ended never gives way.
TEST(TaskTable, full_table_reclaims_the_task_that_ended_first) {
    Tasks tasks(3);
    const uint64_t first = tasks.create();
    const uint64_t second = tasks.create();
    const uint64_t live = tasks.create();
    ASSERT_NE(live, 0U);
    // second ends before first does.
    ASSERT_EQ(tasks.change(second, TaskChange::cancel), ChangeCheck::allowed);
    ASSERT_EQ(tasks.change(first, TaskChange::cancel), ChangeCheck::allowed);
    ASSERT_TRUE(tasks->has_room());

    const uint64_t fourth = tasks.create();
    ASSERT_NE(fourth, 0U);
    EXPECT_FALSE(tasks.exists(second));
    EXPECT_TRUE(tasks.exists(first));
    EXPECT_TRUE(tasks.exists(live));
    EXPECT_EQ(tasks.task(fourth).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(tasks->size(), 3U);

    const uint64_t fifth = tasks.create();
    ASSERT_NE(fifth, 0U);
    EXPECT_FALSE(tasks.exists(first));
    EXPECT_TRUE(tasks.exists(live));
    EXPECT_TRUE(tasks.exists(fourth));
}

TEST(TaskTable, full_table_of_live_tasks_refuses_and_changes_nothing) {
    Tasks tasks(2);
    const uint64_t first = tasks.create();
    const uint64_t second = tasks.create();
    ASSERT_EQ(tasks.change(second, TaskChange::pause), ChangeCheck::allowed);
    EXPECT_FALSE(tasks->has_room());
    EXPECT_EQ(tasks.create(), 0U);
    EXPECT_EQ(tasks->size(), 2U);
    EXPECT_EQ(tasks.task(first).revision, 1U);
    EXPECT_EQ(tasks.task(second).revision, 2U);
}

TEST(TaskTable, live_lifecycles_are_the_ones_not_ended) {
    for (const ayni_task_lifecycle_t lifecycle :
         {AYNI_TASK_READY, AYNI_TASK_RUNNING, AYNI_TASK_PAUSED, AYNI_TASK_CANCELLING}) {
        EXPECT_TRUE(TaskTable::is_live(lifecycle)) << lifecycle;
    }
    for (const ayni_task_lifecycle_t lifecycle :
         {AYNI_TASK_COMPLETED, AYNI_TASK_CANCELLED, AYNI_TASK_FAILED, AYNI_TASK_EXPIRED}) {
        EXPECT_FALSE(TaskTable::is_live(lifecycle)) << lifecycle;
    }
}

} // namespace
