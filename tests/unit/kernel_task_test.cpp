// Tasks through the C ABI (ADR-0018): derivation in ayni_step, the rule that
// a task advances only while its goal, the goal's ancestors and its owner
// allow it, how goal and instance changes reach tasks, TASK_CANCEL and the
// goal it ends with the goal's last task (ADR-0023), instance pause, resume
// and stop, revisions, capacity, the goals that give way with their tasks
// (ADR-0025) and the return values in each kernel state. Runs on the
// simulation Host.

#include "kernel/kernel_state.h"
#include "kernel_node.h"
#include "work_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>

namespace {

using namespace ayni::test;

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();

// Creates a goal owned by agent_id's instance and returns its id.
uint64_t create_goal(WorkNode &work, ayni_trigger_kind_t trigger, uint64_t parent_goal_id = 0,
                     uint64_t agent_id = agent_a, const char *type = "reminder") {
    const Submitted created = work.create(WorkNode::create_body(
        work.instance(agent_id), parent_goal_id, type, 1, {nullptr, 0}, trigger));
    EXPECT_EQ(created.status, AYNI_OK);
    return created.result.object.id;
}

uint64_t once_goal(WorkNode &work, uint64_t parent_goal_id = 0, uint64_t agent_id = agent_a) {
    return create_goal(work, AYNI_TRIGGER_ONCE, parent_goal_id, agent_id);
}

// A goal that only organizes its children.
uint64_t folder_goal(WorkNode &work, uint64_t parent_goal_id = 0) {
    return create_goal(work, AYNI_TRIGGER_NONE, parent_goal_id, agent_a, "folder");
}

// An INTERVAL goal whose first occurrence is due a second after creation.
uint64_t every_second_goal(WorkNode &work) {
    ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a), 0, "reminder", 1,
                                                     {nullptr, 0}, AYNI_TRIGGER_INTERVAL);
    body.trigger_delay_us = AYNI_TRIGGER_PERIOD_MIN_US;
    body.trigger_period_us = AYNI_TRIGGER_PERIOD_MIN_US;
    const Submitted created = work.create(body);
    EXPECT_EQ(created.status, AYNI_OK);
    return created.result.object.id;
}

// The goal's latest task, 0 before the first.
uint64_t task_of(WorkNode &work, uint64_t goal_id) {
    return work.goal(goal_id).last_task_id;
}

uint32_t step_items(WorkNode &work, uint32_t max_items = unbounded_items) {
    return work.node().step(max_items).items_done;
}

uint64_t wakes(WorkNode &work) {
    return work.node().sim().wake_count();
}

bool task_exists(WorkNode &work, uint64_t task_id) {
    ayni_task_info_v1 info{};
    info.struct_size = sizeof(info);
    return ayni_get_task(work.kernel(), task_id, &info) == AYNI_OK;
}

// White-box: puts the task at the largest revision.
void exhaust_task(WorkNode &work, uint64_t task_id) {
    const ayni::work::TaskTable &tasks = work.kernel()->work.tasks;
    uint32_t index = 0;
    ASSERT_TRUE(tasks.find(task_id, index));
    const_cast<ayni::work::Task &>(tasks.at(index)).revision = max_revision;
}

// White-box: puts the goal at the largest revision.
void exhaust_goal(WorkNode &work, uint64_t goal_id) {
    const ayni::work::GoalTable &goals = work.kernel()->work.goals;
    uint32_t index = 0;
    ASSERT_TRUE(goals.find(goal_id, index));
    const_cast<ayni::work::Goal &>(goals.at(index)).revision = max_revision;
}

// --- Derivation -----------------------------------------------------------

// ADR-0018 decisions 4 and 8: the intent creates the goal and wakes the
// owner; the next step derives the one occurrence as a READY task in a
// transition of its own, with an id from the shared allocator. The goal
// records the task and keeps its revision.
TEST(WorkTask, once_goal_derives_one_task_in_the_next_step) {
    WorkNode work;
    work.run();
    const uint64_t wakes_before = wakes(work);
    work.node().sim().advance_us(1000);
    const uint64_t created_at = work.node().sim().now_us();
    const Submitted created = work.create(WorkNode::create_body(
        work.instance(agent_a), 0, "reminder", 1, {nullptr, 0}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_EQ(wakes(work), wakes_before + 1U);
    const uint64_t goal_id = created.result.object.id;
    EXPECT_EQ(work.goal(goal_id).trigger, AYNI_TRIGGER_ONCE);
    EXPECT_EQ(work.goal(goal_id).occurrences, 0U);
    EXPECT_EQ(work.goal(goal_id).next_due_us, created_at);

    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U); // the task awaits its strategy's decision
    const ayni_goal_info_v1 goal = work.goal(goal_id);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goal.occurrences, 1U);
    EXPECT_EQ(goal.revision, 1U);
    ASSERT_NE(goal.last_task_id, 0U);

    const ayni_task_info_v1 task = work.task(goal.last_task_id);
    EXPECT_EQ(task.struct_size, sizeof(task));
    EXPECT_EQ(task.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(task.task_id, goal.last_task_id);
    EXPECT_EQ(task.task_id >> 32U, 1U); // the boot epoch (C-ID-010)
    EXPECT_GT(task.task_id, goal_id);
    EXPECT_EQ(task.goal_id, goal_id);
    EXPECT_EQ(task.owner_instance_id, work.instance(agent_a));
    EXPECT_EQ(task.revision, 1U);
    EXPECT_EQ(task.cancel_revision, 0U);
    EXPECT_EQ(task.occurrence, 1U);
    EXPECT_EQ(task.occurrence_count, 1U);
    EXPECT_EQ(task.due_us, created_at);
    EXPECT_EQ(goal.next_due_us, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(report.next_deadline_us, std::numeric_limits<uint64_t>::max());
    EXPECT_EQ(task.commit_seq, created.result.commit_seq + 1U);
    EXPECT_EQ(task.transition_id, task.commit_seq + 0U); // both start after the boot batch
    EXPECT_EQ(goal.commit_seq, task.commit_seq);
    EXPECT_GT(task.commit_seq, work.node().info().durable_seq);

    // ONCE has no second occurrence: the next step only decides the task
    // (ADR-0022), and the one after has nothing to do.
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(step_items(work), 0U);
    EXPECT_EQ(work.goal(goal_id).occurrences, 1U);
}

TEST(WorkTask, none_goal_derives_no_task_and_wakes_no_one) {
    WorkNode work;
    work.run();
    const uint64_t wakes_before = wakes(work);
    const uint64_t goal_id = create_goal(work, AYNI_TRIGGER_NONE);
    EXPECT_EQ(wakes(work), wakes_before);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(work.goal(goal_id).occurrences, 0U);
    EXPECT_EQ(work.goal(goal_id).last_task_id, 0U);
    EXPECT_EQ(work.goal(goal_id).revision, 1U);
}

// ADR-0018 decision 8: deriving is the kernel's own progress through the
// trigger, so the revision the caller got from GOAL_CREATE still names the
// goal; a goal at the largest revision still derives.
TEST(WorkTask, deriving_keeps_the_goal_revision_the_caller_holds) {
    WorkNode work;
    work.run();
    const Submitted created = work.create(WorkNode::create_body(
        work.instance(agent_a), 0, "reminder", 1, {nullptr, 0}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(created.status, AYNI_OK);
    const uint64_t goal_id = created.result.object.id;
    ASSERT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.goal(goal_id).revision, created.result.object.revision);
    EXPECT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, goal_id, created.result.object.revision).status,
              AYNI_OK);

    const uint64_t exhausted = once_goal(work);
    exhaust_goal(work, exhausted);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.goal(exhausted).occurrences, 1U);
    EXPECT_EQ(work.goal(exhausted).revision, max_revision);
}

// C-LIMIT-011: one derived task is one item, in creation order, and so is
// each decision, made after the derivations of its step (ADR-0022).
TEST(WorkTask, step_derives_one_task_per_item_and_reports_more_ready) {
    WorkNode work;
    work.run();
    const uint64_t goals[] = {once_goal(work), once_goal(work), once_goal(work)};

    ayni_step_report_v1 report = work.node().step(1);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_NE(task_of(work, goals[0]), 0U);
    EXPECT_EQ(task_of(work, goals[1]), 0U);
    report = work.node().step(1);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_NE(task_of(work, goals[1]), 0U);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 3U); // the third task, then the first two decided
    EXPECT_EQ(report.more_ready, 1U); // the third task awaits its decision
    EXPECT_NE(task_of(work, goals[2]), 0U);
    EXPECT_EQ(work.task(task_of(work, goals[1])).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.task(task_of(work, goals[2])).lifecycle, AYNI_TASK_READY);
    EXPECT_LT(task_of(work, goals[0]), task_of(work, goals[1]));
    EXPECT_LT(task_of(work, goals[1]), task_of(work, goals[2]));
}

TEST(WorkTask, step_stops_deriving_once_its_time_budget_is_spent) {
    WorkNode work;
    work.run();
    const uint64_t first = once_goal(work);
    const uint64_t second = once_goal(work);
    work.node().sim().advance_per_read_us(5);
    const ayni_step_report_v1 report = work.node().step(unbounded_items, 5);
    work.node().sim().advance_per_read_us(0);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_NE(task_of(work, first), 0U);
    EXPECT_EQ(task_of(work, second), 0U);
    EXPECT_EQ(step_items(work), 2U); // the second task, and the first one's decision
}

// lifecycles.md: DRAINING takes no new work, and derivation is new work.
TEST(WorkTask, only_running_derives_tasks) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    // White-box: STOPPED answers no query, but the tables are still there.
    const ayni::kernel::WorkModel &model = work.kernel()->work;
    uint32_t index = 0;
    ASSERT_TRUE(model.goals.find(goal_id, index));
    EXPECT_EQ(model.goals.at(index).cursor.taken, 0U);
    EXPECT_EQ(model.tasks.size(), 0U);
}

// ADR-0018 decision 8: without an id the occurrence waits on its goal; with
// no wait limit (ADR-0020) it is never dropped.
TEST(WorkTask, used_up_ids_leave_the_occurrence_waiting) {
    WorkNode work;
    work.run();
    work.kernel()->work.ids.start(1, 0xFFFFFFFFU);
    const uint64_t goal_id = once_goal(work); // takes the last id
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(work.goal(goal_id).occurrences, 0U);
    EXPECT_EQ(work.goal(goal_id).revision, 1U);
}

// ADR-0018 decision 8: derivation takes the first goal in creation order that
// may advance, so a held goal keeps no later goal waiting.
TEST(WorkTask, held_goal_keeps_no_later_goal_from_deriving) {
    WorkNode work;
    work.run();
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
    const uint64_t held = once_goal(work);
    const uint64_t later = once_goal(work, 0, agent_b);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(task_of(work, held), 0U);
    EXPECT_NE(task_of(work, later), 0U);
    // Once the later task is decided, the held goal leaves nothing ready.
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(task_of(work, held), 0U);
}

// ADR-0018 decision 3: an ended task gives its place to a new one when the
// table is full; a task that has not ended never does. The strategy holds
// the tasks, so they stay live.
TEST(WorkTask, full_task_table_reclaims_the_task_that_ended_first) {
    WorkNode work;
    work.node().config().task_capacity = 2;
    work.run();
    work.strategy().answer = cannot_decide;
    const uint64_t first = once_goal(work);
    const uint64_t second = once_goal(work);
    const uint64_t third = once_goal(work);
    EXPECT_EQ(step_items(work), 2U);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 2U); // both decided
    EXPECT_EQ(report.more_ready, 0U); // the third waits for a place
    EXPECT_EQ(task_of(work, third), 0U);

    const uint64_t first_task = task_of(work, first);
    const uint64_t wakes_before = wakes(work);
    ASSERT_EQ(work.cancel_task(first_task).status, AYNI_OK);
    EXPECT_EQ(wakes(work), wakes_before + 1U);
    EXPECT_TRUE(task_exists(work, first_task)); // still queryable until a task needs its place
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_FALSE(task_exists(work, first_task));
    EXPECT_TRUE(task_exists(work, task_of(work, second)));
    EXPECT_EQ(work.task(task_of(work, third)).lifecycle, AYNI_TASK_READY);

    // Both places hold live tasks again; the step only decides the third.
    const uint64_t fourth = once_goal(work);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(task_of(work, fourth), 0U);
}

// --- Goals giving way ------------------------------------------------------

// ADR-0025: a full goal table gives the place of the goal that ended first,
// whenever it was created, to the new goal, and the goal's tasks leave the
// task table with it. Their ids then name nothing. Goals that are not
// terminal keep their places, and a table of them refuses the next goal.
TEST(WorkTask, goal_that_ended_first_gives_way_with_its_tasks) {
    WorkNode work;
    work.node().config().goal_capacity = 3;
    work.run();
    work.strategy().answer = cannot_decide;
    const uint64_t later = once_goal(work);
    const uint64_t earlier = once_goal(work);
    const uint64_t live = once_goal(work);
    ASSERT_EQ(step_items(work), 3U);
    ASSERT_EQ(step_items(work), 3U); // each task held
    const uint64_t later_task = task_of(work, later);
    const uint64_t earlier_task = task_of(work, earlier);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, earlier).status, AYNI_OK);
    ASSERT_EQ(work.cancel_task(later_task).status, AYNI_OK); // and with it the goal
    ASSERT_EQ(work.goal(later).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_TRUE(task_exists(work, earlier_task)); // until a goal needs the place

    const uint64_t first_created = work.create_goal();
    EXPECT_TRUE(task_exists(work, later_task));
    EXPECT_EQ(work.goal(later).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_FALSE(task_exists(work, earlier_task));
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_goal(work.kernel(), earlier, &info), AYNI_NOT_FOUND);
    expect_refused(work.change(AYNI_INTENT_GOAL_CANCEL, earlier, 2), AYNI_NOT_FOUND,
                   AYNI_REASON_GOAL_UNKNOWN, AYNI_OBJECT_GOAL, earlier);
    expect_refused(work.cancel_task(earlier_task, 3), AYNI_NOT_FOUND, AYNI_REASON_TASK_UNKNOWN,
                   AYNI_OBJECT_TASK, earlier_task);
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a), earlier)),
                   AYNI_NOT_FOUND, AYNI_REASON_PARENT_UNKNOWN, AYNI_OBJECT_GOAL, earlier);

    const uint64_t second_created = work.create_goal();
    EXPECT_FALSE(task_exists(work, later_task));
    EXPECT_EQ(ayni_get_goal(work.kernel(), later, &info), AYNI_NOT_FOUND);
    EXPECT_EQ(work.task(task_of(work, live)).lifecycle, AYNI_TASK_PAUSED);
    for (const uint64_t goal_id : {live, first_created, second_created}) {
        EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE) << goal_id;
    }
    expect_refused(work.create(WorkNode::create_body(work.instance(agent_a))),
                   AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GOAL_CAPACITY);
}

// ADR-0025: a child may take a place before its parent's; its parent's
// pause, resume and cancel still reach it and its task.
TEST(WorkTask, child_in_an_earlier_place_follows_its_parent) {
    WorkNode work;
    work.node().config().goal_capacity = 2;
    work.run();
    const uint64_t ended = folder_goal(work);
    const uint64_t parent = folder_goal(work);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, ended).status, AYNI_OK);
    const uint64_t child = once_goal(work, parent);
    const ayni::work::GoalTable &goals = work.kernel()->work.goals;
    uint32_t child_index = 0;
    uint32_t parent_index = 0;
    ASSERT_TRUE(goals.find(child, child_index));
    ASSERT_TRUE(goals.find(parent, parent_index));
    ASSERT_LT(child_index, parent_index);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, child);

    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, parent).status, AYNI_OK);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, parent).status, AYNI_OK);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_READY);
    const Submitted cancelled = work.change(AYNI_INTENT_GOAL_CANCEL, parent);
    ASSERT_EQ(cancelled.status, AYNI_OK);
    EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(work.task(task_id).commit_seq, cancelled.result.commit_seq);
}

// --- Goal changes reach their tasks ---------------------------------------

// ADR-0018 decision 5: pausing a goal does not rewrite its descendants, but
// their tasks stop with it and continue once every goal above them is ACTIVE
// again; only tasks whose state changes get a new revision.
TEST(WorkTask, paused_ancestor_holds_the_tasks_below_it_until_resumed) {
    WorkNode work;
    work.run();
    const uint64_t root = folder_goal(work);
    const uint64_t child = once_goal(work, root);
    const uint64_t grandchild = once_goal(work, child);
    ASSERT_EQ(step_items(work), 2U);
    const uint64_t child_task = task_of(work, child);
    const uint64_t grandchild_task = task_of(work, grandchild);

    const Submitted paused = work.change(AYNI_INTENT_GOAL_PAUSE, root);
    ASSERT_EQ(paused.status, AYNI_OK);
    for (const uint64_t task_id : {child_task, grandchild_task}) {
        const ayni_task_info_v1 task = work.task(task_id);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED) << task_id;
        EXPECT_EQ(task.revision, 2U) << task_id;
        EXPECT_EQ(task.commit_seq, paused.result.commit_seq) << task_id;
    }
    EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(child).revision, 1U);

    // Pausing the child as well changes no task, and resuming the root
    // leaves both tasks held by the paused child.
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, child).status, AYNI_OK);
    EXPECT_EQ(work.task(child_task).revision, 2U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, root).status, AYNI_OK);
    EXPECT_EQ(work.task(child_task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(grandchild_task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(grandchild_task).revision, 2U);

    const Submitted resumed = work.change(AYNI_INTENT_GOAL_RESUME, child);
    ASSERT_EQ(resumed.status, AYNI_OK);
    for (const uint64_t task_id : {child_task, grandchild_task}) {
        const ayni_task_info_v1 task = work.task(task_id);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_READY) << task_id;
        EXPECT_EQ(task.revision, 3U) << task_id;
        EXPECT_EQ(task.commit_seq, resumed.result.commit_seq) << task_id;
    }
}

TEST(WorkTask, goal_created_under_a_paused_parent_derives_once_the_parent_resumes) {
    WorkNode work;
    work.run();
    const uint64_t root = folder_goal(work);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, root).status, AYNI_OK);
    const uint64_t child = once_goal(work, root);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(task_of(work, child), 0U);

    const uint64_t wakes_before = wakes(work);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, root).status, AYNI_OK);
    EXPECT_EQ(wakes(work), wakes_before + 1U);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.task(task_of(work, child)).lifecycle, AYNI_TASK_READY);
}

TEST(WorkTask, paused_goal_holds_its_own_task) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);

    const uint64_t wakes_before = wakes(work);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    EXPECT_EQ(wakes(work), wakes_before); // pausing lets nothing derive
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(task_id).revision, 3U);
}

// ADR-0018 decision 5: a task needs its goal and its owner at once, so
// releasing either one alone leaves it held.
TEST(WorkTask, task_waits_for_both_its_goal_and_its_owner) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);

    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_a).status, AYNI_OK);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task_id).revision, 2U);

    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(task_id).revision, 3U);
}

// C-LIFE-022: cancelling a goal cancels the tasks of the goal and of every
// descendant in the same transition; an ended task keeps its revision.
TEST(WorkTask, cancel_reaches_the_tasks_of_the_goal_and_its_descendants) {
    WorkNode work;
    work.run();
    const uint64_t root = once_goal(work);
    const uint64_t child = once_goal(work, root);
    const uint64_t grandchild = once_goal(work, child);
    const uint64_t other = once_goal(work);
    ASSERT_EQ(step_items(work), 4U);
    const uint64_t ended = task_of(work, grandchild);
    const Submitted ended_cancel = work.cancel_task(ended);
    ASSERT_EQ(ended_cancel.status, AYNI_OK);

    const Submitted cancelled = work.change(AYNI_INTENT_GOAL_CANCEL, root);
    ASSERT_EQ(cancelled.status, AYNI_OK);
    for (const uint64_t goal_id : {root, child}) {
        const ayni_task_info_v1 task = work.task(task_of(work, goal_id));
        EXPECT_EQ(task.lifecycle, AYNI_TASK_CANCELLED) << goal_id;
        EXPECT_EQ(task.revision, 2U) << goal_id;
        EXPECT_EQ(task.cancel_revision, 2U) << goal_id;
        EXPECT_EQ(task.commit_seq, cancelled.result.commit_seq) << goal_id;
    }
    EXPECT_EQ(work.task(ended).revision, 2U);
    EXPECT_EQ(work.task(ended).commit_seq, ended_cancel.result.commit_seq);
    EXPECT_EQ(work.task(task_of(work, other)).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(task_of(work, other)).revision, 1U);
}

// --- TASK_CANCEL ----------------------------------------------------------

// ADR-0023: TASK_CANCEL of a ONCE goal's one task cancels the goal in the
// same transition, with its descendants and their tasks, as GOAL_CANCEL
// would; other goals keep their revision.
TEST(WorkTask, task_cancel_of_a_one_shot_goals_task_cancels_the_goal) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = once_goal(work);
    const uint64_t child = once_goal(work, goal_id);
    const uint64_t folder = folder_goal(work, goal_id);
    const uint64_t other = once_goal(work);
    ASSERT_EQ(step_items(work), 3U);
    const uint64_t task_id = task_of(work, goal_id);
    const ayni_task_info_v1 other_task = work.task(task_of(work, other));

    const Submitted cancelled = work.cancel_task(task_id);
    ASSERT_EQ(cancelled.status, AYNI_OK);
    EXPECT_EQ(cancelled.result.object.kind, AYNI_OBJECT_TASK);
    EXPECT_EQ(cancelled.result.object.id, task_id);
    EXPECT_EQ(cancelled.result.object.revision, 2U);
    const ayni_task_info_v1 task = work.task(task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(task.cancel_revision, 2U);
    EXPECT_EQ(task.commit_seq, cancelled.result.commit_seq);
    for (const uint64_t ended : {goal_id, child, folder}) {
        const ayni_goal_info_v1 goal = work.goal(ended);
        EXPECT_EQ(goal.lifecycle, AYNI_GOAL_CANCELLED) << ended;
        EXPECT_EQ(goal.revision, 2U) << ended;
        EXPECT_EQ(goal.cancel_revision, 2U) << ended;
        EXPECT_EQ(goal.commit_seq, cancelled.result.commit_seq) << ended;
    }
    const ayni_task_info_v1 child_task = work.task(task_of(work, child));
    EXPECT_EQ(child_task.lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(child_task.cancel_revision, 2U);
    EXPECT_EQ(child_task.commit_seq, cancelled.result.commit_seq);
    EXPECT_EQ(work.goal(other).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(other).revision, 1U);
    EXPECT_EQ(work.task(task_of(work, other)).commit_seq, other_task.commit_seq);
    EXPECT_EQ(step_items(work), 1U); // only the other goal's task is decided
    EXPECT_EQ(work.goal(goal_id).occurrences, 1U);
}

// The goal of a PAUSED task may itself be PAUSED, or held back by a paused
// ancestor or owner, and the task may be held by its strategy's decision as
// well; the goal is cancelled all the same.
TEST(WorkTask, task_cancel_cancels_a_one_shot_goal_whatever_holds_its_task) {
    for (int holder = 0; holder < 4; ++holder) {
        SCOPED_TRACE(holder);
        WorkNode work;
        work.run();
        const uint64_t root = folder_goal(work);
        const uint64_t goal_id = once_goal(work, root);
        ASSERT_EQ(step_items(work), 1U);
        const uint64_t task_id = task_of(work, goal_id);
        if (holder == 3) {
            work.strategy().answer = cannot_decide;
            ASSERT_EQ(step_items(work), 1U);
        }
        if (holder == 2) {
            ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
        } else {
            const uint64_t paused = holder == 1 ? root : goal_id;
            ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, paused).status, AYNI_OK);
        }
        ASSERT_EQ(work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
        const ayni_goal_info_v1 before = work.goal(goal_id);

        const Submitted cancelled = work.cancel_task(task_id);
        ASSERT_EQ(cancelled.status, AYNI_OK);
        const ayni_goal_info_v1 goal = work.goal(goal_id);
        EXPECT_EQ(goal.lifecycle, AYNI_GOAL_CANCELLED);
        EXPECT_EQ(goal.revision, before.revision + 1U);
        EXPECT_EQ(goal.commit_seq, cancelled.result.commit_seq);
        EXPECT_EQ(work.goal(root).lifecycle, holder == 1 ? AYNI_GOAL_PAUSED : AYNI_GOAL_ACTIVE);
    }
}

// ADR-0023: the goal ends with its last task only, once no occurrence is to
// come and no other task has not ended; a recurring goal goes on, its
// revision staying.
TEST(WorkTask, task_cancel_ends_a_goal_only_with_its_last_task) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = every_second_goal(work);
    work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
    ASSERT_EQ(step_items(work, 1), 1U);
    const uint64_t first = task_of(work, goal_id);
    const ayni_goal_info_v1 before = work.goal(goal_id);
    ASSERT_EQ(work.cancel_task(first).status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(goal_id).revision, before.revision);
    EXPECT_EQ(work.goal(goal_id).commit_seq, before.commit_seq);

    work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
    ASSERT_EQ(step_items(work, 1), 1U);
    const uint64_t second_task = task_of(work, goal_id);
    work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
    ASSERT_EQ(step_items(work, 1), 1U);
    const uint64_t third_task = task_of(work, goal_id);
    // White-box: the third occurrence was the last.
    uint32_t index = 0;
    ASSERT_TRUE(work.kernel()->work.goals.find(goal_id, index));
    const_cast<ayni::work::Goal &>(work.kernel()->work.goals.at(index)).cursor.next_due_us =
        UINT64_MAX;
    ASSERT_EQ(work.cancel_task(second_task).status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(goal_id).revision, before.revision);
    const Submitted last = work.cancel_task(third_task);
    ASSERT_EQ(last.status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(work.goal(goal_id).commit_seq, last.result.commit_seq);
}

// C-COMMIT-012: the task, then its revision, then its lifecycle.
TEST(WorkTask, task_cancel_checks_the_task_then_its_revision_then_its_state) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);

    expect_refused(work.cancel_task(task_id + 100U, 1), AYNI_NOT_FOUND, AYNI_REASON_TASK_UNKNOWN,
                   AYNI_OBJECT_TASK, task_id + 100U);
    Submitted refused = work.cancel_task(task_id, 2);
    expect_refused(refused, AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION, AYNI_OBJECT_TASK,
                   task_id);
    EXPECT_EQ(refused.error.object.revision, 1U);
    ASSERT_EQ(work.cancel_task(task_id).status, AYNI_OK);
    // Stale and ended at once: the revision comes first.
    expect_refused(work.cancel_task(task_id, 1), AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION,
                   AYNI_OBJECT_TASK, task_id);
    refused = work.cancel_task(task_id, 2);
    expect_refused(refused, AYNI_INVALID_STATE, AYNI_REASON_TASK_STATE, AYNI_OBJECT_TASK, task_id);
    EXPECT_EQ(refused.error.object.revision, 2U);
    EXPECT_EQ(work.task(task_id).revision, 2U);
}

// --- Instances ------------------------------------------------------------

TEST(WorkTask, instance_pause_holds_its_tasks_and_resume_releases_them) {
    WorkNode work;
    work.run();
    const uint64_t mine = once_goal(work);
    const uint64_t theirs = once_goal(work, 0, agent_b);
    ASSERT_EQ(step_items(work), 2U);
    const uint64_t instance = work.instance(agent_a);

    uint64_t wakes_before = wakes(work);
    const Submitted paused = work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a);
    ASSERT_EQ(paused.status, AYNI_OK);
    EXPECT_EQ(wakes(work), wakes_before);
    EXPECT_EQ(paused.result.object.kind, AYNI_OBJECT_AGENT_INSTANCE);
    EXPECT_EQ(paused.result.object.id, instance);
    EXPECT_EQ(paused.result.object.revision, 2U);
    const ayni_agent_info_v1 agent = work.agent(agent_a);
    EXPECT_EQ(agent.lifecycle, AYNI_AGENT_PAUSED);
    EXPECT_EQ(agent.revision, 2U);
    EXPECT_EQ(agent.commit_seq, paused.result.commit_seq);
    EXPECT_EQ(work.task(task_of(work, mine)).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task_of(work, mine)).commit_seq, paused.result.commit_seq);
    EXPECT_EQ(work.goal(mine).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.task(task_of(work, theirs)).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(task_of(work, theirs)).revision, 1U);

    wakes_before = wakes(work);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_a).status, AYNI_OK);
    EXPECT_EQ(wakes(work), wakes_before + 1U);
    EXPECT_EQ(work.agent(agent_a).lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(work.agent(agent_a).revision, 3U);
    EXPECT_EQ(work.task(task_of(work, mine)).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(task_of(work, mine)).revision, 3U);
}

// ADR-0018 decision 7: a PAUSED owner still takes goals; their tasks wait
// until it resumes.
TEST(WorkTask, paused_instance_takes_goals_and_derives_after_resume) {
    WorkNode work;
    work.run();
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
    const uint64_t goal_id = once_goal(work);
    EXPECT_EQ(step_items(work), 0U);
    EXPECT_EQ(task_of(work, goal_id), 0U);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_a).status, AYNI_OK);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.task(task_of(work, goal_id)).lifecycle, AYNI_TASK_READY);
}

// ADR-0018 decision 6: stopping cancels every goal and task the instance
// owns in the same transition; STOPPED has no exit and takes no goal.
TEST(WorkTask, instance_stop_cancels_its_goals_and_tasks) {
    WorkNode work;
    work.run();
    const uint64_t root = once_goal(work);
    const uint64_t child = folder_goal(work, root);
    const uint64_t theirs = once_goal(work, 0, agent_b);
    ASSERT_EQ(step_items(work), 2U);
    const uint64_t instance = work.instance(agent_a);
    // A PAUSED instance stops too.
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);

    const Submitted stopped = work.change_agent(AYNI_INTENT_INSTANCE_STOP, agent_a);
    ASSERT_EQ(stopped.status, AYNI_OK);
    EXPECT_EQ(stopped.result.object.revision, 3U);
    EXPECT_EQ(work.agent(agent_a).lifecycle, AYNI_AGENT_STOPPED);
    for (const uint64_t goal_id : {root, child}) {
        const ayni_goal_info_v1 goal = work.goal(goal_id);
        EXPECT_EQ(goal.lifecycle, AYNI_GOAL_CANCELLED) << goal_id;
        EXPECT_EQ(goal.cancel_revision, goal.revision) << goal_id;
        EXPECT_EQ(goal.commit_seq, stopped.result.commit_seq) << goal_id;
    }
    const ayni_task_info_v1 task = work.task(task_of(work, root));
    EXPECT_EQ(task.lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(task.cancel_revision, 3U); // paused with the instance, then cancelled
    EXPECT_EQ(task.commit_seq, stopped.result.commit_seq);
    EXPECT_EQ(work.goal(theirs).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.task(task_of(work, theirs)).lifecycle, AYNI_TASK_READY);

    for (const ayni_intent_kind_t kind :
         {AYNI_INTENT_INSTANCE_PAUSE, AYNI_INTENT_INSTANCE_RESUME, AYNI_INTENT_INSTANCE_STOP}) {
        const Submitted refused = work.change_agent(kind, agent_a);
        expect_refused(refused, AYNI_INVALID_STATE, AYNI_REASON_INSTANCE_STATE,
                       AYNI_OBJECT_AGENT_INSTANCE, instance);
        EXPECT_EQ(refused.error.object.revision, 3U);
    }
    const Submitted refused = work.create(WorkNode::create_body(instance));
    expect_refused(refused, AYNI_INVALID_STATE, AYNI_REASON_OWNER_STATE, AYNI_OBJECT_AGENT_INSTANCE,
                   instance);
    EXPECT_EQ(refused.error.object.revision, 3U);
}

// C-COMMIT-012: the instance, then its revision, then its lifecycle.
TEST(WorkTask, instance_intents_check_the_instance_then_its_revision_then_its_state) {
    WorkNode work;
    work.run();
    const uint64_t instance = work.instance(agent_a);
    for (const uint64_t unknown : {uint64_t{0}, instance + 100U}) {
        expect_refused(work.change_instance(AYNI_INTENT_INSTANCE_PAUSE, unknown, 1), AYNI_NOT_FOUND,
                       AYNI_REASON_INSTANCE_UNKNOWN, AYNI_OBJECT_AGENT_INSTANCE, unknown);
    }
    Submitted refused = work.change_instance(AYNI_INTENT_INSTANCE_PAUSE, instance, 2);
    expect_refused(refused, AYNI_REVISION_CONFLICT, AYNI_REASON_INSTANCE_REVISION,
                   AYNI_OBJECT_AGENT_INSTANCE, instance);
    EXPECT_EQ(refused.error.object.revision, 1U);
    // Stale and in the wrong state at once: the revision comes first.
    expect_refused(work.change_instance(AYNI_INTENT_INSTANCE_RESUME, instance, 2),
                   AYNI_REVISION_CONFLICT, AYNI_REASON_INSTANCE_REVISION,
                   AYNI_OBJECT_AGENT_INSTANCE, instance);
    refused = work.change_instance(AYNI_INTENT_INSTANCE_RESUME, instance, 1);
    expect_refused(refused, AYNI_INVALID_STATE, AYNI_REASON_INSTANCE_STATE,
                   AYNI_OBJECT_AGENT_INSTANCE, instance);
    EXPECT_EQ(work.agent(agent_a).revision, 1U);
}

// --- Goal creation --------------------------------------------------------

// ADR-0018 decision 4: the goal type, then whether it accepts the trigger,
// then the owner.
TEST(WorkTask, goal_type_must_accept_the_trigger) {
    WorkNode work;
    work.run();
    const uint64_t owner = work.instance(agent_a);
    expect_refused(
        work.create(WorkNode::create_body(owner, 0, "folder", 1, {nullptr, 0}, AYNI_TRIGGER_ONCE)),
        AYNI_INVALID_ARGUMENT, AYNI_REASON_TRIGGER_KIND, AYNI_OBJECT_GOAL_TYPE, 0);
    expect_refused(work.create(WorkNode::create_body(owner + 100U, 0, "folder", 1, {nullptr, 0},
                                                     AYNI_TRIGGER_ONCE)),
                   AYNI_INVALID_ARGUMENT, AYNI_REASON_TRIGGER_KIND, AYNI_OBJECT_GOAL_TYPE, 0);
    expect_refused(
        work.create(WorkNode::create_body(owner, 0, "alarm", 1, {nullptr, 0}, AYNI_TRIGGER_ONCE)),
        AYNI_NOT_FOUND, AYNI_REASON_GOAL_TYPE_UNKNOWN, AYNI_OBJECT_GOAL_TYPE, 0);
    // Nothing above used an id.
    const uint64_t folder = folder_goal(work);
    EXPECT_EQ(folder, (uint64_t{1} << 32U) | 4U); // two instances and the session before it
}

// --- Revisions ------------------------------------------------------------

// ADR-0018 decision 9: an intent that would raise a revision past the
// largest value, a task's included, changes nothing.
TEST(WorkTask, change_that_would_exhaust_a_task_revision_is_refused) {
    WorkNode work;
    work.run();
    const uint64_t root = folder_goal(work);
    const uint64_t goal_id = once_goal(work, root);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    exhaust_task(work, task_id);
    const ayni_agent_info_v1 agent_before = work.agent(agent_a);

    expect_refused(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id), AYNI_CAPACITY_EXCEEDED,
                   AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_GOAL, goal_id);
    expect_refused(work.change(AYNI_INTENT_GOAL_CANCEL, root), AYNI_CAPACITY_EXCEEDED,
                   AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_GOAL, root);
    expect_refused(work.cancel_task(task_id, max_revision), AYNI_CAPACITY_EXCEEDED,
                   AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_TASK, task_id);
    const uint64_t instance = work.instance(agent_a);
    for (const ayni_intent_kind_t kind : {AYNI_INTENT_INSTANCE_PAUSE, AYNI_INTENT_INSTANCE_STOP}) {
        expect_refused(work.change_agent(kind, agent_a), AYNI_CAPACITY_EXCEEDED,
                       AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_AGENT_INSTANCE, instance);
    }
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(goal_id).revision, 1U);
    EXPECT_EQ(work.goal(root).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(root).revision, 1U);
    EXPECT_EQ(work.agent(agent_a).revision, agent_before.revision);
    EXPECT_EQ(work.agent(agent_a).commit_seq, agent_before.commit_seq);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_READY);
}

// ADR-0023: an INTERVAL goal whose next time would be past the clock's
// range has no occurrence to come either, so it ends with its task.
TEST(WorkTask, task_cancel_ends_an_interval_goal_whose_next_time_never_comes) {
    WorkNode work;
    work.run();
    ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a), 0, "reminder", 1,
                                                     {nullptr, 0}, AYNI_TRIGGER_INTERVAL);
    body.trigger_period_us = UINT64_MAX;
    const Submitted created = work.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    const uint64_t goal_id = created.result.object.id;
    ASSERT_EQ(step_items(work), 1U);
    ASSERT_EQ(work.goal(goal_id).next_due_us, UINT64_MAX);
    ASSERT_EQ(work.cancel_task(task_of(work, goal_id)).status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_CANCELLED);
}

// ADR-0023: a TASK_CANCEL that ends its goal raises every revision the
// goal's end raises, so one that would raise any of them past the largest,
// the goal's, a descendant's or a descendant's task's, is refused. One that
// leaves the goal as it is goes through.
TEST(WorkTask, task_cancel_that_would_exhaust_a_revision_its_goals_end_raises_is_refused) {
    for (int exhausted = 0; exhausted < 3; ++exhausted) {
        SCOPED_TRACE(exhausted);
        WorkNode work;
        work.run();
        const uint64_t goal_id = once_goal(work);
        const uint64_t child =
            exhausted == 2 ? once_goal(work, goal_id) : folder_goal(work, goal_id);
        ASSERT_EQ(step_items(work), exhausted == 2 ? 2U : 1U);
        const uint64_t task_id = task_of(work, goal_id);
        switch (exhausted) {
        case 0:
            exhaust_goal(work, goal_id);
            break;
        case 1:
            exhaust_goal(work, child);
            break;
        default:
            exhaust_task(work, task_of(work, child));
            break;
        }
        const ayni_goal_info_v1 before = work.goal(goal_id);

        const Submitted refused = work.cancel_task(task_id);
        expect_refused(refused, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED,
                       AYNI_OBJECT_TASK, task_id);
        EXPECT_EQ(refused.error.object.revision, 1U);
        EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_READY);
        EXPECT_EQ(work.task(task_id).revision, 1U);
        EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(work.goal(goal_id).commit_seq, before.commit_seq);
        EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_ACTIVE);
    }

    WorkNode work;
    work.run();
    const uint64_t goal_id = every_second_goal(work);
    work.node().sim().advance_us(AYNI_TRIGGER_PERIOD_MIN_US);
    ASSERT_EQ(step_items(work, 1), 1U);
    exhaust_goal(work, goal_id);
    EXPECT_EQ(work.cancel_task(task_of(work, goal_id)).status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(goal_id).revision, max_revision);
}

// Resuming raises the revision of each task it releases, so a held task at
// the largest revision refuses the resume of its goal or of its owner.
TEST(WorkTask, resume_that_would_exhaust_a_task_revision_is_refused) {
    WorkNode work;
    work.run();
    const uint64_t mine = once_goal(work);
    const uint64_t theirs = once_goal(work, 0, agent_b);
    ASSERT_EQ(step_items(work), 2U);
    const uint64_t my_task = task_of(work, mine);
    const uint64_t their_task = task_of(work, theirs);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, mine).status, AYNI_OK);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_b).status, AYNI_OK);
    exhaust_task(work, my_task);
    exhaust_task(work, their_task);

    expect_refused(work.change(AYNI_INTENT_GOAL_RESUME, mine), AYNI_CAPACITY_EXCEEDED,
                   AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_GOAL, mine);
    EXPECT_EQ(work.goal(mine).lifecycle, AYNI_GOAL_PAUSED);
    EXPECT_EQ(work.task(my_task).lifecycle, AYNI_TASK_PAUSED);
    expect_refused(work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_b), AYNI_CAPACITY_EXCEEDED,
                   AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_AGENT_INSTANCE,
                   work.instance(agent_b));
    EXPECT_EQ(work.agent(agent_b).lifecycle, AYNI_AGENT_PAUSED);
    EXPECT_EQ(work.task(their_task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(their_task).revision, max_revision);
}

// Only tasks whose state changes get a new revision, so a change that leaves
// a task at the largest revision as it is goes through.
TEST(WorkTask, change_that_leaves_an_exhausted_task_as_it_is_is_allowed) {
    WorkNode work;
    work.run();
    const uint64_t root = folder_goal(work);
    const uint64_t goal_id = once_goal(work, root);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    exhaust_task(work, task_id);
    EXPECT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, root).status, AYNI_OK);
    EXPECT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, root).status, AYNI_OK);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.task(task_id).revision, max_revision);
}

// Stopping cancels the instance's goals, so a goal at the largest revision
// refuses the stop even without tasks.
TEST(WorkTask, stop_that_would_exhaust_a_goal_revision_is_refused) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = folder_goal(work);
    exhaust_goal(work, goal_id);
    const uint64_t instance = work.instance(agent_a);
    expect_refused(work.change_agent(AYNI_INTENT_INSTANCE_STOP, agent_a), AYNI_CAPACITY_EXCEEDED,
                   AYNI_REASON_REVISION_EXHAUSTED, AYNI_OBJECT_AGENT_INSTANCE, instance);
    EXPECT_EQ(work.agent(agent_a).lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
    // Pausing does not change goals.
    EXPECT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
}

// --- Wakes ----------------------------------------------------------------

// ADR-0018 decision 8: an intent that may let ayni_step derive a task wakes
// the owner before it returns; one that cannot, or is refused, does not.
TEST(WorkTask, intents_that_may_let_a_task_derive_wake_the_owner) {
    WorkNode work;
    work.run();
    work.strategy().answer = cannot_decide; // the tasks stay live
    const uint64_t goal_id = once_goal(work);
    // Its own goal, which the cancel of its one task cancels too.
    const uint64_t task_goal = once_goal(work);
    ASSERT_EQ(step_items(work), 2U);
    const uint64_t task_id = task_of(work, task_goal);
    const uint64_t other = once_goal(work, 0, agent_b);
    ASSERT_EQ(step_items(work), 3U); // the other goal's task, and the holds

    const struct {
        const char *name;
        std::function<Submitted()> submit;
        bool wakes;
    } cases[] = {
        {"goal pause", [&] { return work.change(AYNI_INTENT_GOAL_PAUSE, goal_id); }, false},
        {"refused pause", [&] { return work.change(AYNI_INTENT_GOAL_PAUSE, goal_id); }, false},
        {"goal resume", [&] { return work.change(AYNI_INTENT_GOAL_RESUME, goal_id); }, true},
        {"task cancel", [&] { return work.cancel_task(task_id); }, true},
        {"goal cancel", [&] { return work.change(AYNI_INTENT_GOAL_CANCEL, goal_id); }, true},
        {"instance pause", [&] { return work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_b); },
         false},
        {"instance resume", [&] { return work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_b); },
         true},
        {"instance stop", [&] { return work.change_agent(AYNI_INTENT_INSTANCE_STOP, agent_b); },
         true},
    };
    for (const auto &c : cases) {
        SCOPED_TRACE(c.name);
        const uint64_t before = wakes(work);
        static_cast<void>(c.submit());
        EXPECT_EQ(wakes(work), before + (c.wakes ? 1U : 0U));
    }
    EXPECT_EQ(work.goal(other).lifecycle, AYNI_GOAL_CANCELLED);
}

// --- Arguments and states -------------------------------------------------

TEST(WorkTask, task_and_instance_bodies_are_checked_before_anything_else) {
    WorkNode work;
    work.run();
    ayni_task_target_v1 task_target{};
    task_target.struct_size = sizeof(task_target);
    ayni_instance_target_v1 instance_target{};
    instance_target.struct_size = sizeof(instance_target);

    ayni_task_target_v1 short_task = task_target;
    short_task.struct_size -= 1U;
    ayni_task_target_v1 reserved_task = task_target;
    reserved_task.reserved = 1;
    ayni_instance_target_v1 short_instance = instance_target;
    short_instance.struct_size -= 1U;
    ayni_instance_target_v1 reserved_instance = instance_target;
    reserved_instance.reserved = 1;
    const struct {
        ayni_intent_kind_t kind;
        const void *body;
    } invalid[] = {
        {AYNI_INTENT_TASK_CANCEL, &short_task},
        {AYNI_INTENT_TASK_CANCEL, &reserved_task},
        {AYNI_INTENT_INSTANCE_PAUSE, &short_instance},
        {AYNI_INTENT_INSTANCE_RESUME, &reserved_instance},
        {AYNI_INTENT_INSTANCE_STOP, &reserved_instance},
        {AYNI_INTENT_TASK_CANCEL, nullptr},
    };
    for (const auto &entry : invalid) {
        SCOPED_TRACE(entry.kind);
        ayni_intent_v1 intent = work.intent(entry.kind, entry.body, 1);
        // Even through a closed session and with a command id.
        intent.session_id = 999;
        intent.command_id = 5;
        expect_refused(work.submit(intent), AYNI_INVALID_ARGUMENT, AYNI_REASON_NONE);
    }
    task_target.struct_size += 8U;
    EXPECT_EQ(work.submit(work.intent(AYNI_INTENT_TASK_CANCEL, &task_target, 1)).status,
              AYNI_UNSUPPORTED_VERSION);
    instance_target.struct_size += 8U;
    EXPECT_EQ(work.submit(work.intent(AYNI_INTENT_INSTANCE_STOP, &instance_target, 1)).status,
              AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(work.agent(agent_a).revision, 1U);
}

TEST(WorkTask, task_query_checks_its_arguments) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ayni_task_info_v1 info{};
    EXPECT_EQ(ayni_get_task(nullptr, task_id, &info), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_get_task(work.kernel(), task_id, nullptr), AYNI_INVALID_ARGUMENT);
    info.struct_size = sizeof(info) - 1U;
    EXPECT_EQ(ayni_get_task(work.kernel(), task_id, &info), AYNI_INVALID_ARGUMENT);
    // The 0.9 layout, before the hold, is not written either.
    info.struct_size = offsetof(ayni_task_info_v1, hold_status);
    EXPECT_EQ(ayni_get_task(work.kernel(), task_id, &info), AYNI_INVALID_ARGUMENT);
    info.struct_size = sizeof(info) + 8U;
    EXPECT_EQ(ayni_get_task(work.kernel(), task_id, &info), AYNI_UNSUPPORTED_VERSION);
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_task(work.kernel(), task_id + 1U, &info), AYNI_NOT_FOUND);
    EXPECT_EQ(ayni_get_task(work.kernel(), 0, &info), AYNI_NOT_FOUND);
}

// C-LIFE-004: outside RUNNING these intents are NOT_READY and not queued.
TEST(WorkTask, task_and_instance_intents_outside_running_are_not_ready) {
    for (const ayni_lifecycle_t state :
         {AYNI_LIFECYCLE_CONFIGURING, AYNI_LIFECYCLE_RECOVERING, AYNI_LIFECYCLE_RESTRICTED,
          AYNI_LIFECYCLE_DRAINING, AYNI_LIFECYCLE_STOPPED}) {
        SCOPED_TRACE(state);
        WorkNode work;
        work.node().enter(state);
        ayni_task_target_v1 task_target{};
        task_target.struct_size = sizeof(task_target);
        task_target.task_id = 1;
        expect_refused(work.submit(work.intent(AYNI_INTENT_TASK_CANCEL, &task_target, 1)),
                       AYNI_NOT_READY, AYNI_REASON_NONE);
        ayni_instance_target_v1 instance_target{};
        instance_target.struct_size = sizeof(instance_target);
        instance_target.instance_id = 1;
        for (const ayni_intent_kind_t kind :
             {AYNI_INTENT_INSTANCE_PAUSE, AYNI_INTENT_INSTANCE_RESUME, AYNI_INTENT_INSTANCE_STOP}) {
            expect_refused(work.submit(work.intent(kind, &instance_target, 1)), AYNI_NOT_READY,
                           AYNI_REASON_NONE);
        }
        EXPECT_EQ(work.node().lifecycle(), state);
    }
}

} // namespace
