// Tasks owned by work/task (ADR-0018, ADR-0022): revisions and the cancel
// revision (C-COMMIT-012), the transitions this part reaches, holds, the
// order in which READY tasks are decided, and the places ended tasks give up
// when the table is full.

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
    uint64_t create(uint32_t occurrence = 1, uint32_t goal_index = 3) {
        const uint64_t task_id = last_id_ + 1U;
        if (!table_.insert({goal_id, owner, 900, goal_index, occurrence, 2}, task_id,
                           next_stamp())) {
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
    for (const TaskChange kind :
         {TaskChange::pause, TaskChange::resume, TaskChange::cancel, TaskChange::expire}) {
        EXPECT_EQ(tasks.change(id, kind), ChangeCheck::invalid_state);
    }
    EXPECT_EQ(tasks.task(id).revision, 5U);
}

// ADR-0020: a READY or PAUSED task expires with its goal; EXPIRED has no exit
// and, unlike CANCELLED, no cancel revision.
TEST(TaskTable, ready_and_paused_tasks_expire) {
    Tasks tasks(2);
    const uint64_t ready = tasks.create();
    const uint64_t paused = tasks.create();
    ASSERT_EQ(tasks.change(paused, TaskChange::pause), ChangeCheck::allowed);
    for (const uint64_t id : {ready, paused}) {
        const uint32_t revision = tasks.task(id).revision;
        ASSERT_EQ(tasks.change(id, TaskChange::expire), ChangeCheck::allowed);
        EXPECT_EQ(tasks.task(id).lifecycle, AYNI_TASK_EXPIRED);
        EXPECT_EQ(tasks.task(id).revision, revision + 1U);
        EXPECT_EQ(tasks.task(id).cancel_revision, 0U);
        EXPECT_EQ(tasks.task(id).commit_seq, tasks.last_seq());
        for (const TaskChange kind :
             {TaskChange::pause, TaskChange::resume, TaskChange::cancel, TaskChange::expire}) {
            EXPECT_EQ(tasks.change(id, kind), ChangeCheck::invalid_state);
        }
    }
    // An expired task has ended, so it gives its place to a new one.
    EXPECT_NE(tasks.create(), 0U);
    EXPECT_FALSE(tasks.exists(ready));
}

// ADR-0022: a decision ends a READY task as COMPLETED or FAILED, neither of
// which has an exit or a cancel revision.
TEST(TaskTable, decisions_complete_or_fail_a_ready_task) {
    Tasks tasks(3);
    const uint64_t completed = tasks.create();
    const uint64_t failed = tasks.create();
    const uint64_t paused = tasks.create();
    ASSERT_EQ(tasks.change(paused, TaskChange::pause), ChangeCheck::allowed);
    EXPECT_EQ(tasks.change(paused, TaskChange::complete), ChangeCheck::invalid_state);
    EXPECT_EQ(tasks.change(paused, TaskChange::fail), ChangeCheck::invalid_state);

    ASSERT_EQ(tasks.change(completed, TaskChange::complete), ChangeCheck::allowed);
    ASSERT_EQ(tasks.change(failed, TaskChange::fail), ChangeCheck::allowed);
    EXPECT_EQ(tasks.task(completed).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(tasks.task(failed).lifecycle, AYNI_TASK_FAILED);
    for (const uint64_t id : {completed, failed}) {
        EXPECT_EQ(tasks.task(id).revision, 2U);
        EXPECT_EQ(tasks.task(id).cancel_revision, 0U);
        for (const TaskChange kind : {TaskChange::pause, TaskChange::resume, TaskChange::cancel,
                                      TaskChange::expire, TaskChange::complete, TaskChange::fail}) {
            EXPECT_EQ(tasks.change(id, kind), ChangeCheck::invalid_state);
        }
    }
    // Both have ended, so the one that ended first gives its place.
    EXPECT_NE(tasks.create(), 0U);
    EXPECT_FALSE(tasks.exists(completed));
    EXPECT_TRUE(tasks.exists(failed));
}

// ADR-0022: a held task is PAUSED with the reason; nothing but an end takes
// it out, and the reason stays once it has ended.
TEST(TaskTable, held_task_stays_paused_until_it_ends) {
    Tasks tasks(2);
    const uint64_t id = tasks.create();
    EXPECT_FALSE(TaskTable::is_held(tasks.task(id)));
    EXPECT_EQ(tasks.task(id).hold_status, AYNI_OK);
    EXPECT_EQ(tasks.task(id).hold_reason, AYNI_REASON_NONE);
    tasks->hold(tasks.index_of(id), AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION, {40, 30});
    const Task &held = tasks.task(id);
    EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_TRUE(TaskTable::is_held(held));
    EXPECT_EQ(held.hold_status, AYNI_REVISION_CONFLICT);
    EXPECT_EQ(held.hold_reason, AYNI_REASON_TASK_REVISION);
    EXPECT_EQ(held.revision, 2U);
    EXPECT_EQ(held.commit_seq, 30U);
    for (const TaskChange kind :
         {TaskChange::pause, TaskChange::resume, TaskChange::complete, TaskChange::fail}) {
        EXPECT_EQ(tasks.change(id, kind), ChangeCheck::invalid_state);
    }
    ASSERT_EQ(tasks.change(id, TaskChange::cancel), ChangeCheck::allowed);
    EXPECT_EQ(tasks.task(id).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(tasks.task(id).hold_status, AYNI_REVISION_CONFLICT);
    EXPECT_EQ(tasks.task(id).hold_reason, AYNI_REASON_TASK_REVISION);

    const uint64_t expiring = tasks.create();
    tasks->hold(tasks.index_of(expiring), AYNI_UNSUPPORTED, AYNI_REASON_NONE, {50, 40});
    EXPECT_EQ(tasks.change(expiring, TaskChange::expire), ChangeCheck::allowed);
}

// ADR-0022: a task is still held from its hold until it ends, and the table
// knows whether any task is without looking at them.
TEST(TaskTable, task_is_still_held_from_its_hold_until_it_ends) {
    Tasks tasks(4);
    const uint64_t held = tasks.create();
    const uint64_t other = tasks.create();
    const uint64_t paused = tasks.create();
    EXPECT_FALSE(tasks->has_held());
    ASSERT_EQ(tasks.change(paused, TaskChange::pause), ChangeCheck::allowed);
    EXPECT_FALSE(TaskTable::still_held(tasks.task(paused)));
    tasks->hold(tasks.index_of(held), AYNI_UNSUPPORTED, AYNI_REASON_NONE, {40, 30});
    tasks->hold(tasks.index_of(other), AYNI_INTERNAL, AYNI_REASON_NONE, {41, 31});
    EXPECT_TRUE(tasks->has_held());
    EXPECT_TRUE(TaskTable::still_held(tasks.task(held)));

    ASSERT_EQ(tasks.change(held, TaskChange::cancel), ChangeCheck::allowed);
    EXPECT_FALSE(TaskTable::still_held(tasks.task(held)));
    EXPECT_TRUE(TaskTable::is_held(tasks.task(held)));
    EXPECT_TRUE(tasks->has_held());
    ASSERT_EQ(tasks.change(other, TaskChange::expire), ChangeCheck::allowed);
    EXPECT_FALSE(tasks->has_held());
}

// ADR-0022: the next task to decide is the one READY longest, by commit_seq
// and then id, among those READY before the bound and below the largest
// revision.
TEST(TaskTable, next_ready_takes_the_task_ready_longest_before_the_bound) {
    Tasks tasks(4);
    const uint64_t first = tasks.create();
    const uint64_t second = tasks.create();
    const uint64_t third = tasks.create();
    uint32_t index = 99;
    EXPECT_FALSE(tasks->next_ready(1, index));
    ASSERT_TRUE(tasks->next_ready(2, index));
    EXPECT_EQ(index, tasks.index_of(first));

    // Paused and resumed, first is READY again only since then.
    ASSERT_EQ(tasks.change(first, TaskChange::pause), ChangeCheck::allowed);
    ASSERT_TRUE(tasks->next_ready(tasks.last_seq() + 1U, index));
    EXPECT_EQ(index, tasks.index_of(second));
    ASSERT_EQ(tasks.change(first, TaskChange::resume), ChangeCheck::allowed);
    tasks.raw(second).revision = std::numeric_limits<uint32_t>::max();
    ASSERT_TRUE(tasks->next_ready(tasks.last_seq() + 1U, index));
    EXPECT_EQ(index, tasks.index_of(third));

    // One transition may make several tasks READY: the lower id goes first.
    tasks.raw(third).commit_seq = tasks.task(first).commit_seq;
    ASSERT_TRUE(tasks->next_ready(tasks.last_seq() + 1U, index));
    EXPECT_EQ(index, tasks.index_of(first));
}

TEST(TaskTable, has_ready_counts_tasks_below_the_largest_revision) {
    Tasks tasks(2);
    EXPECT_FALSE(tasks->has_ready());
    const uint64_t id = tasks.create();
    EXPECT_TRUE(tasks->has_ready());
    tasks.raw(id).revision = std::numeric_limits<uint32_t>::max();
    EXPECT_FALSE(tasks->has_ready());
    tasks.raw(id).revision = 1;
    ASSERT_EQ(tasks.change(id, TaskChange::pause), ChangeCheck::allowed);
    EXPECT_FALSE(tasks->has_ready());
}

TEST(TaskTable, live_task_besides_looks_at_the_other_tasks_of_the_goal) {
    Tasks tasks(3);
    const uint64_t first = tasks.create();
    const uint64_t second = tasks.create();
    const uint32_t goal_index = tasks.task(first).goal_index;
    EXPECT_TRUE(tasks->live_task_besides(goal_index, tasks.index_of(first)));
    EXPECT_FALSE(tasks->live_task_besides(goal_index + 1U, tasks.index_of(first)));
    ASSERT_EQ(tasks.change(second, TaskChange::pause), ChangeCheck::allowed);
    EXPECT_TRUE(tasks->live_task_besides(goal_index, tasks.index_of(first)));
    ASSERT_EQ(tasks.change(second, TaskChange::cancel), ChangeCheck::allowed);
    EXPECT_FALSE(tasks->live_task_besides(goal_index, tasks.index_of(first)));
    EXPECT_TRUE(tasks->live_task_besides(goal_index, tasks.index_of(second)));
    // no_index sets no task aside.
    EXPECT_TRUE(tasks->live_task_besides(goal_index, TaskTable::no_index));
    ASSERT_EQ(tasks.change(first, TaskChange::cancel), ChangeCheck::allowed);
    EXPECT_FALSE(tasks->live_task_besides(goal_index, TaskTable::no_index));
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
