// Goal expiry and bounded waits through the C ABI (ADR-0020): goals that
// expire with their descendants and tasks, occurrences skipped once their
// wait for a task ends, a goal that fails when its last occurrence is
// skipped (ADR-0023), the reasons goal info gives, and the deadline
// ayni_step reports. Runs on the simulation Host, whose clock moves only when
// a test moves it.

#include "kernel/kernel_state.h"
#include "kernel_node.h"
#include "work_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

using namespace ayni::test;

constexpr uint64_t no_time = std::numeric_limits<uint64_t>::max();
constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();
constexpr uint64_t second = AYNI_TRIGGER_PERIOD_MIN_US;

struct GoalSpec {
    ayni_trigger_kind_t trigger = AYNI_TRIGGER_NONE;
    uint64_t delay_us = 0;
    uint64_t period_us = 0;
    uint64_t expiry_us = 0;
    uint64_t parent_goal_id = 0;
    uint64_t agent_id = agent_a;
    const char *type = "timer"; // waits as long as timer_wait_max_us
};

uint64_t create(WorkNode &work, const GoalSpec &spec) {
    ayni_goal_create_v1 body =
        WorkNode::create_body(work.instance(spec.agent_id), spec.parent_goal_id, spec.type, 1,
                              {nullptr, 0}, spec.trigger);
    body.trigger_delay_us = spec.delay_us;
    body.trigger_period_us = spec.period_us;
    body.expiry_us = spec.expiry_us;
    const Submitted created = work.create(body);
    EXPECT_EQ(created.status, AYNI_OK);
    return created.result.object.id;
}

GoalSpec expiring(uint64_t expiry_us, ayni_trigger_kind_t trigger = AYNI_TRIGGER_NONE,
                  uint64_t parent_goal_id = 0) {
    GoalSpec spec;
    spec.trigger = trigger;
    spec.expiry_us = expiry_us;
    spec.parent_goal_id = parent_goal_id;
    return spec;
}

GoalSpec once(uint64_t parent_goal_id = 0, uint64_t agent_id = agent_a) {
    GoalSpec spec;
    spec.trigger = AYNI_TRIGGER_ONCE;
    spec.parent_goal_id = parent_goal_id;
    spec.agent_id = agent_id;
    return spec;
}

GoalSpec delayed(uint64_t delay_us, uint64_t agent_id = agent_a) {
    GoalSpec spec;
    spec.trigger = AYNI_TRIGGER_DELAY;
    spec.delay_us = delay_us;
    spec.agent_id = agent_id;
    return spec;
}

GoalSpec every_second(uint64_t delay_us = second) {
    GoalSpec spec;
    spec.trigger = AYNI_TRIGGER_INTERVAL;
    spec.delay_us = delay_us;
    spec.period_us = second;
    return spec;
}

// A goal whose one task takes a place in the task table; its "reminder"
// type waits without limit.
uint64_t place_holder(WorkNode &work, uint64_t agent_id = agent_b) {
    GoalSpec spec = once(0, agent_id);
    spec.type = "reminder";
    const uint64_t goal_id = create(work, spec);
    EXPECT_EQ(work.node().step().items_done, 1U);
    // The strategy holds the task, so it stays live until a test ends it.
    work.strategy().answer = cannot_decide;
    EXPECT_EQ(work.node().step().items_done, 1U);
    work.strategy().answer = nullptr;
    return goal_id;
}

uint64_t now(WorkNode &work) {
    return work.node().sim().now_us();
}

void advance(WorkNode &work, uint64_t delta_us) {
    work.node().sim().advance_us(delta_us);
}

void advance_to(WorkNode &work, uint64_t time_us) {
    ASSERT_GE(time_us, now(work));
    advance(work, time_us - now(work));
}

uint64_t wakes(WorkNode &work) {
    return work.node().sim().wake_count();
}

uint64_t task_of(WorkNode &work, uint64_t goal_id) {
    return work.goal(goal_id).last_task_id;
}

// White-box: leaves exactly one id for this boot.
void leave_one_id(WorkNode &work) {
    work.kernel()->work.ids.start(1, 0xFFFFFFFFU);
}

// White-box: puts the goal at the largest revision.
void exhaust_goal(WorkNode &work, uint64_t goal_id) {
    const ayni::work::GoalTable &goals = work.kernel()->work.goals;
    uint32_t index = 0;
    ASSERT_TRUE(goals.find(goal_id, index));
    const_cast<ayni::work::Goal &>(goals.at(index)).revision = max_revision;
}

// White-box: the interval goal's next occurrence is its last.
void make_next_occurrence_the_last(WorkNode &work, uint64_t goal_id) {
    const ayni::work::GoalTable &goals = work.kernel()->work.goals;
    uint32_t index = 0;
    ASSERT_TRUE(goals.find(goal_id, index));
    const_cast<ayni::work::Goal &>(goals.at(index)).cursor.taken = UINT32_MAX - 1U;
}

// White-box: puts the task at the largest revision.
void exhaust_task(WorkNode &work, uint64_t task_id) {
    const ayni::work::TaskTable &tasks = work.kernel()->work.tasks;
    uint32_t index = 0;
    ASSERT_TRUE(tasks.find(task_id, index));
    const_cast<ayni::work::Task &>(tasks.at(index)).revision = max_revision;
}

// --- Expiry ----------------------------------------------------------------

// ADR-0020: a goal created with an expiry becomes EXPIRED at that time; the
// intent wakes the owner to learn it even when the goal has no occurrence.
TEST(WorkExpiry, goal_expires_at_its_time) {
    WorkNode work;
    work.run();
    advance(work, 1000);
    const uint64_t created_at = now(work);
    const uint64_t wakes_before = wakes(work);
    const uint64_t goal_id = create(work, expiring(5U * second));
    EXPECT_EQ(wakes(work), wakes_before + 1U);
    const uint64_t expires_at = created_at + 5U * second;
    const ayni_goal_info_v1 created = work.goal(goal_id);
    EXPECT_EQ(created.expires_us, expires_at);
    EXPECT_EQ(created.lifecycle, AYNI_GOAL_ACTIVE);

    EXPECT_EQ(work.node().step().next_deadline_us, expires_at);
    advance_to(work, expires_at - 1U);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, expires_at);

    advance(work, 1);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    const ayni_goal_info_v1 expired = work.goal(goal_id);
    EXPECT_EQ(expired.lifecycle, AYNI_GOAL_EXPIRED);
    EXPECT_EQ(expired.revision, created.revision + 1U);
    EXPECT_GT(expired.commit_seq, created.commit_seq);
    EXPECT_EQ(expired.cancel_revision, 0U);
    EXPECT_EQ(expired.expires_us, no_time);
    EXPECT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, goal_id).status, AYNI_INVALID_STATE);
}

// ADR-0020: without an expiry, or with one past the clock's range, the goal
// never expires, and an intent creating such a goal with no occurrence
// wakes no one.
TEST(WorkExpiry, expiry_past_the_clock_never_comes) {
    WorkNode work;
    work.run();
    advance(work, 1000);
    const uint64_t wakes_before = wakes(work);
    const uint64_t never = create(work, expiring(0));
    const uint64_t beyond = create(work, expiring(no_time - 1U));
    EXPECT_EQ(wakes(work), wakes_before);
    EXPECT_EQ(work.goal(never).expires_us, no_time);
    EXPECT_EQ(work.goal(beyond).expires_us, no_time);
    EXPECT_EQ(work.node().step().next_deadline_us, no_time);
}

// ADR-0020 decision 3: expiry takes the goal's live descendants and their
// tasks that have not ended, READY or PAUSED, in one transition, raising
// each revision by one; ended work and other goals keep their revision.
TEST(WorkExpiry, expiry_takes_live_descendants_and_their_tasks) {
    WorkNode work;
    work.node().config().task_capacity = 5;
    work.run();
    const uint64_t start = now(work);
    const uint64_t root = create(work, expiring(5U * second));
    const uint64_t child = create(work, once(root));
    const uint64_t grandchild = create(work, once(child));
    const uint64_t done_child = create(work, once(root));
    const uint64_t other = create(work, once());
    EXPECT_EQ(work.node().step().items_done, 4U);
    // The strategy holds the tasks, so they stay live, PAUSED.
    work.strategy().answer = cannot_decide;
    EXPECT_EQ(work.node().step().items_done, 4U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, grandchild).status, AYNI_OK);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, done_child).status, AYNI_OK);
    const ayni_goal_info_v1 done_before = work.goal(done_child);
    const uint32_t revisions_before[] = {work.goal(root).revision, work.goal(child).revision,
                                         work.goal(grandchild).revision};
    const ayni_task_info_v1 paused_task = work.task(task_of(work, grandchild));
    ASSERT_EQ(paused_task.lifecycle, AYNI_TASK_PAUSED);
    const ayni_task_info_v1 other_task = work.task(task_of(work, other));
    // Derived in the step before the expiry, this task is still READY when
    // the expiry comes: a step does its due work before its decisions.
    const uint64_t ready_child = create(work, once(root));
    EXPECT_EQ(work.node().step().items_done, 1U);
    const ayni_task_info_v1 ready_task = work.task(task_of(work, ready_child));
    ASSERT_EQ(ready_task.lifecycle, AYNI_TASK_READY);

    advance_to(work, start + 5U * second);
    EXPECT_EQ(work.node().step().items_done, 1U);
    const uint64_t seq = work.goal(root).commit_seq;
    const uint64_t expired_goals[] = {root, child, grandchild};
    for (std::size_t i = 0; i < 3U; ++i) {
        const ayni_goal_info_v1 goal = work.goal(expired_goals[i]);
        EXPECT_EQ(goal.lifecycle, AYNI_GOAL_EXPIRED) << i;
        EXPECT_EQ(goal.commit_seq, seq) << i;
        EXPECT_EQ(goal.revision, revisions_before[i] + 1U) << i;
    }
    EXPECT_EQ(work.goal(ready_child).lifecycle, AYNI_GOAL_EXPIRED);
    for (const uint64_t goal_id : {child, grandchild, ready_child}) {
        const ayni_task_info_v1 task = work.task(task_of(work, goal_id));
        EXPECT_EQ(task.lifecycle, AYNI_TASK_EXPIRED) << goal_id;
        EXPECT_EQ(task.commit_seq, seq) << goal_id;
        EXPECT_EQ(task.cancel_revision, 0U) << goal_id;
    }
    EXPECT_EQ(work.task(task_of(work, grandchild)).revision, paused_task.revision + 1U);
    EXPECT_EQ(work.task(task_of(work, ready_child)).revision, ready_task.revision + 1U);
    const ayni_goal_info_v1 done_after = work.goal(done_child);
    EXPECT_EQ(done_after.lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(done_after.revision, done_before.revision);
    EXPECT_EQ(work.task(task_of(work, done_child)).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(work.goal(other).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.task(task_of(work, other)).revision, other_task.revision);
}

// A descendant's own earlier expiry ends it alone.
TEST(WorkExpiry, child_expiry_leaves_its_parent) {
    WorkNode work;
    work.run();
    const uint64_t parent = create(work, expiring(0));
    const uint64_t child = create(work, expiring(second, AYNI_TRIGGER_ONCE, parent));
    EXPECT_EQ(work.node().step().items_done, 1U);
    const ayni_goal_info_v1 parent_before = work.goal(parent);

    advance(work, second);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_EXPIRED);
    EXPECT_EQ(work.task(task_of(work, child)).lifecycle, AYNI_TASK_EXPIRED);
    EXPECT_EQ(work.goal(parent).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(parent).revision, parent_before.revision);
    EXPECT_EQ(work.goal(parent).commit_seq, parent_before.commit_seq);
}

// ADR-0020: a pause holds tasks, not the expiry: a paused goal, and a goal
// whose instance is paused, still expire. The pause wakes no one, and the
// next step still reports the expiry.
TEST(WorkExpiry, paused_goal_still_expires) {
    WorkNode work;
    work.run();
    const uint64_t start = now(work);
    const uint64_t paused = create(work, expiring(2U * second));
    GoalSpec held = expiring(2U * second);
    held.agent_id = agent_b;
    const uint64_t held_by_instance = create(work, held);
    const uint64_t wakes_before = wakes(work);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, paused).status, AYNI_OK);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_b).status, AYNI_OK);
    EXPECT_EQ(wakes(work), wakes_before);
    EXPECT_EQ(work.node().step().next_deadline_us, start + 2U * second);

    advance(work, 2U * second);
    EXPECT_EQ(work.node().step().items_done, 2U);
    EXPECT_EQ(work.goal(paused).lifecycle, AYNI_GOAL_EXPIRED);
    EXPECT_EQ(work.goal(held_by_instance).lifecycle, AYNI_GOAL_EXPIRED);
    // Of equal expiries, the goal created first expires first.
    EXPECT_LT(work.goal(paused).commit_seq, work.goal(held_by_instance).commit_seq);
}

// ADR-0020: a goal that expires takes no occurrence due in the same step;
// one taken by an earlier step expires with it as a task.
TEST(WorkExpiry, expiry_wins_over_an_occurrence_due_in_the_same_step) {
    WorkNode work;
    work.run();
    GoalSpec spec = delayed(second);
    spec.expiry_us = 2U * second;
    const uint64_t in_time = create(work, spec);
    const uint64_t too_late = create(work, spec);
    advance(work, second);
    ASSERT_EQ(ayni_step(work.kernel(), 1, 1000000, nullptr), AYNI_OK); // the first goal only
    EXPECT_EQ(work.goal(in_time).occurrences, 1U);
    const uint64_t task_id = task_of(work, in_time);

    advance(work, 2U * second);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 2U);
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_EXPIRED);
    const ayni_goal_info_v1 goal = work.goal(too_late);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_EXPIRED);
    EXPECT_EQ(goal.occurrences, 0U);
    EXPECT_EQ(goal.last_task_id, 0U);
    EXPECT_EQ(goal.next_due_us, no_time);
}

// ADR-0020: the deadline is the earlier of the next occurrence and the
// expiry.
TEST(WorkExpiry, expiry_counts_in_the_deadline) {
    WorkNode work;
    work.run();
    const uint64_t start = now(work);
    GoalSpec spec = every_second();
    spec.expiry_us = 2U * second + second / 2U;
    const uint64_t goal_id = create(work, spec);
    EXPECT_EQ(work.node().step().next_deadline_us, start + second);
    advance_to(work, start + second);
    EXPECT_EQ(work.node().step().next_deadline_us, start + 2U * second);
    advance_to(work, start + 2U * second);
    EXPECT_EQ(work.node().step().next_deadline_us, start + spec.expiry_us);
    advance_to(work, start + spec.expiry_us);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    EXPECT_EQ(work.goal(goal_id).occurrences, 2U);
    EXPECT_EQ(work.task(task_of(work, goal_id)).lifecycle, AYNI_TASK_EXPIRED);
}

// ADR-0020: expiring needs neither a place in the task table nor an id, so
// its deadline stays while the occurrences have none.
TEST(WorkExpiry, expiry_needs_neither_room_nor_an_id) {
    WorkNode work;
    work.node().config().task_capacity = 1;
    work.run();
    place_holder(work);
    leave_one_id(work);
    GoalSpec spec = delayed(second);
    spec.expiry_us = 3U * second;
    const uint64_t goal_id = create(work, spec); // takes the last id
    const uint64_t expires_at = work.goal(goal_id).expires_us;
    EXPECT_EQ(work.node().step().next_deadline_us, expires_at);

    advance_to(work, expires_at);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_EXPIRED);
    EXPECT_EQ(work.goal(goal_id).occurrences_skipped, 0U);
}

// ADR-0020: expiry comes first in a step, so the place an expired task gives
// up serves an occurrence waiting in the same step.
TEST(WorkExpiry, expiry_makes_room_for_a_waiting_occurrence) {
    WorkNode work;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t start = now(work);
    const uint64_t holder = create(work, expiring(2U * second, AYNI_TRIGGER_ONCE));
    EXPECT_EQ(work.node().step().items_done, 1U);
    const uint64_t waiting = create(work, delayed(second, agent_b));

    advance(work, 3U * second);
    EXPECT_EQ(work.node().step().items_done, 2U);
    // The expired task gave its place to the new one.
    EXPECT_EQ(work.goal(holder).lifecycle, AYNI_GOAL_EXPIRED);
    ayni_task_info_v1 gone{};
    gone.struct_size = sizeof(gone);
    EXPECT_EQ(ayni_get_task(work.kernel(), task_of(work, holder), &gone), AYNI_NOT_FOUND);
    const ayni_task_info_v1 task = work.task(task_of(work, waiting));
    EXPECT_EQ(task.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(task.due_us, start + second);
}

// C-COMMIT-012: no revision passes the largest, so an expiry that would raise
// one, the goal's own, a descendant's or a task's, is dropped: the goal keeps
// its lifecycle and revision and never expires.
TEST(WorkExpiry, expiry_that_would_exhaust_a_revision_is_dropped) {
    WorkNode work;
    work.run();
    const uint64_t own = create(work, expiring(second));
    const uint64_t with_task = create(work, expiring(second));
    const uint64_t task_goal = create(work, once(with_task));
    const uint64_t with_child = create(work, expiring(second));
    const uint64_t child = create(work, expiring(0, AYNI_TRIGGER_NONE, with_child));
    EXPECT_EQ(work.node().step().items_done, 1U);
    exhaust_goal(work, own);
    exhaust_task(work, task_of(work, task_goal));
    exhaust_goal(work, child);

    advance(work, second);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 3U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    for (const uint64_t goal_id : {own, with_task, with_child}) {
        const ayni_goal_info_v1 goal = work.goal(goal_id);
        EXPECT_EQ(goal.lifecycle, AYNI_GOAL_ACTIVE) << goal_id;
        EXPECT_EQ(goal.expires_us, no_time) << goal_id;
        EXPECT_EQ(goal.revision, goal_id == own ? max_revision : 1U) << goal_id;
    }
    EXPECT_GT(work.goal(own).commit_seq, work.goal(task_goal).commit_seq);
    EXPECT_EQ(work.task(task_of(work, task_goal)).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_ACTIVE);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
}

// A step that starts with the clock at its largest value finds no due work
// where there is none: that value is also the never of no candidate.
TEST(WorkExpiry, clock_at_its_largest_value_finds_no_due_work) {
    WorkNode work;
    work.run();
    advance_to(work, no_time);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    const uint64_t goal_id = create(work, expiring(0));
    report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
}

// --- Bounded waits -----------------------------------------------------------

// C-RES-005, ADR-0020 decision 5: while the task table has no room, a due
// occurrence waits for its goal type's limit, counted from when it fell due;
// then every occurrence due by then is skipped and counted with the reason,
// the revision staying.
TEST(WorkWait, occurrences_are_skipped_once_their_wait_ends) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t start = now(work);
    const uint64_t holder = place_holder(work);
    const uint64_t goal_id = create(work, every_second());
    const ayni_goal_info_v1 created = work.goal(goal_id);
    EXPECT_EQ(created.wait_reason, AYNI_REASON_TASK_CAPACITY);
    EXPECT_EQ(created.skip_reason, AYNI_REASON_NONE);
    EXPECT_EQ(work.node().step().next_deadline_us, start + 3U * second);
    advance_to(work, start + 3U * second - 1U);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, start + 3U * second);

    advance(work, 1);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, start + 6U * second);
    const ayni_goal_info_v1 skipped = work.goal(goal_id);
    EXPECT_EQ(skipped.occurrences, 3U);
    EXPECT_EQ(skipped.occurrences_skipped, 3U);
    EXPECT_EQ(skipped.skip_reason, AYNI_REASON_TASK_CAPACITY);
    EXPECT_EQ(skipped.last_task_id, 0U);
    EXPECT_EQ(skipped.next_due_us, start + 4U * second);
    EXPECT_EQ(skipped.revision, created.revision);
    EXPECT_GT(skipped.commit_seq, created.commit_seq);
    EXPECT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id, created.revision).status, AYNI_OK);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);

    // With room again, the next occurrence becomes a task; the count and
    // reason of the skip stay.
    ASSERT_EQ(work.cancel_task(task_of(work, holder)).status, AYNI_OK);
    EXPECT_EQ(work.node().step().next_deadline_us, start + 4U * second);
    advance_to(work, start + 4U * second);
    EXPECT_EQ(work.node().step().items_done, 1U);
    const ayni_goal_info_v1 derived = work.goal(goal_id);
    EXPECT_EQ(work.task(derived.last_task_id).occurrence, 4U);
    EXPECT_EQ(work.task(derived.last_task_id).occurrence_count, 1U);
    EXPECT_EQ(derived.occurrences_skipped, 3U);
    EXPECT_EQ(derived.skip_reason, AYNI_REASON_TASK_CAPACITY);
}

// With no id left no task can be derived until the next boot, so the wait
// ends with ID_EXHAUSTED. That was the goal's one occurrence, so it fails
// (ADR-0023).
TEST(WorkWait, used_up_ids_skip_with_their_reason) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.run();
    const uint64_t start = now(work);
    leave_one_id(work);
    const uint64_t goal_id = create(work, delayed(second)); // takes the last id
    EXPECT_EQ(work.goal(goal_id).wait_reason, AYNI_REASON_ID_EXHAUSTED);
    EXPECT_EQ(work.node().step().next_deadline_us, start + 3U * second);

    advance(work, 3U * second);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    const ayni_goal_info_v1 goal = work.goal(goal_id);
    EXPECT_EQ(goal.occurrences_skipped, 1U);
    EXPECT_EQ(goal.skip_reason, AYNI_REASON_ID_EXHAUSTED);
    EXPECT_EQ(goal.wait_reason, AYNI_REASON_NONE); // none to come
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_FAILED);
}

// ADR-0023: a skip that leaves a goal with no occurrence to come and no task
// that has not ended makes it FAILED in the same transition, keeping the
// skip's count and reason, and cancels its live descendants with their
// tasks, as a failed last task would.
TEST(WorkWait, skipping_the_last_occurrence_fails_the_goal) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.node().config().task_capacity = 2;
    work.run();
    const uint64_t start = now(work);
    const uint64_t goal_id = create(work, delayed(second));
    GoalSpec child_spec = once(goal_id);
    child_spec.type = "reminder"; // waits without limit
    const uint64_t child = create(work, child_spec);
    GoalSpec holder_spec = once(0, agent_b);
    holder_spec.type = "reminder";
    const uint64_t holder = create(work, holder_spec);
    // The strategy holds both tasks, so they fill the table.
    work.strategy().answer = cannot_decide;
    ASSERT_EQ(work.node().step().items_done, 2U);
    ASSERT_EQ(work.node().step().items_done, 2U);
    const uint64_t child_task = task_of(work, child);
    ASSERT_EQ(work.task(child_task).lifecycle, AYNI_TASK_PAUSED);
    const ayni_goal_info_v1 created = work.goal(goal_id);

    advance_to(work, start + 3U * second);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    const ayni_goal_info_v1 failed = work.goal(goal_id);
    EXPECT_EQ(failed.lifecycle, AYNI_GOAL_FAILED);
    EXPECT_EQ(failed.revision, created.revision + 1U);
    EXPECT_EQ(failed.cancel_revision, 0U);
    EXPECT_EQ(failed.occurrences, 1U);
    EXPECT_EQ(failed.occurrences_skipped, 1U);
    EXPECT_EQ(failed.skip_reason, AYNI_REASON_TASK_CAPACITY);
    EXPECT_EQ(failed.wait_reason, AYNI_REASON_NONE);
    EXPECT_EQ(failed.next_due_us, no_time);
    EXPECT_EQ(failed.last_task_id, 0U);
    const ayni_goal_info_v1 cancelled = work.goal(child);
    EXPECT_EQ(cancelled.lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(cancelled.cancel_revision, cancelled.revision);
    EXPECT_EQ(cancelled.commit_seq, failed.commit_seq);
    EXPECT_EQ(work.task(child_task).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(work.task(child_task).commit_seq, failed.commit_seq);
    EXPECT_EQ(work.goal(holder).lifecycle, AYNI_GOAL_ACTIVE);
    // The revision the caller held is stale now.
    expect_refused(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id, created.revision),
                   AYNI_REVISION_CONFLICT, AYNI_REASON_GOAL_REVISION, AYNI_OBJECT_GOAL, goal_id);
    // The skip and the end took one commit_seq.
    const Submitted next = work.cancel_task(task_of(work, holder));
    ASSERT_EQ(next.status, AYNI_OK);
    EXPECT_EQ(next.result.commit_seq, failed.commit_seq + 1U);
}

// A goal skipped past its last occurrence while a task of its own has not
// ended stays ACTIVE; that task's end then ends it (ADR-0023).
TEST(WorkWait, skip_leaves_a_goal_with_a_live_task_active) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.run();
    const uint64_t goal_id = create(work, every_second());
    work.strategy().answer = cannot_decide;
    advance(work, second);
    ASSERT_EQ(work.node().step().items_done, 1U);
    ASSERT_EQ(work.node().step().items_done, 1U);
    const uint64_t held = task_of(work, goal_id);
    ASSERT_EQ(work.task(held).lifecycle, AYNI_TASK_PAUSED);
    make_next_occurrence_the_last(work, goal_id);

    advance(work, 3U * second);
    ASSERT_EQ(work.node().step().items_done, 1U);
    const ayni_goal_info_v1 skipped = work.goal(goal_id);
    EXPECT_EQ(skipped.lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(skipped.occurrences, UINT32_MAX);
    EXPECT_EQ(skipped.occurrences_skipped, 1U);
    EXPECT_EQ(skipped.skip_reason, AYNI_REASON_TASK_HELD);
    EXPECT_EQ(skipped.next_due_us, no_time);
    const Submitted cancelled = work.cancel_task(held);
    ASSERT_EQ(cancelled.status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(work.goal(goal_id).commit_seq, cancelled.result.commit_seq);
}

// C-COMMIT-012: a goal whose end would raise a revision past the largest,
// its own or a descendant's, is only skipped and stays ACTIVE with nothing
// to come, as for an expiry that is dropped.
TEST(WorkWait, skip_whose_end_would_exhaust_a_revision_leaves_the_goal_active) {
    for (const bool own : {true, false}) {
        SCOPED_TRACE(own ? "own" : "descendant");
        WorkNode work;
        work.timer_wait_max_us = 0;
        work.node().config().task_capacity = 1;
        work.run();
        place_holder(work);
        const uint64_t goal_id = create(work, delayed(second));
        GoalSpec child_spec;
        child_spec.parent_goal_id = goal_id;
        const uint64_t child = create(work, child_spec);
        exhaust_goal(work, own ? goal_id : child);
        const ayni_goal_info_v1 before = work.goal(goal_id);

        advance(work, second);
        EXPECT_EQ(work.node().step().items_done, 1U);
        const ayni_goal_info_v1 skipped = work.goal(goal_id);
        EXPECT_EQ(skipped.lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(skipped.revision, before.revision);
        EXPECT_EQ(skipped.occurrences_skipped, 1U);
        EXPECT_EQ(skipped.skip_reason, AYNI_REASON_TASK_CAPACITY);
        EXPECT_EQ(skipped.next_due_us, no_time);
        EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(work.node().step().items_done, 0U);
    }
}

// A wait limit of 0 skips a due occurrence in the step that finds no room,
// and changes nothing while there is room.
TEST(WorkWait, zero_wait_skips_at_once) {
    WorkNode work;
    work.timer_wait_max_us = 0;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t start = now(work);
    const uint64_t holder = place_holder(work);
    const uint64_t goal_id = create(work, every_second());
    EXPECT_EQ(work.node().step().next_deadline_us, start + second);
    advance(work, second);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, start + 2U * second);
    EXPECT_EQ(work.goal(goal_id).occurrences_skipped, 1U);

    ASSERT_EQ(work.cancel_task(task_of(work, holder)).status, AYNI_OK);
    advance(work, second);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(work.goal(goal_id).occurrences_skipped, 1U);
    EXPECT_EQ(work.task(task_of(work, goal_id)).occurrence, 2U);
}

// AYNI_OCCURRENCE_WAIT_UNBOUNDED keeps occurrences waiting with no deadline
// until a task can take them all.
TEST(WorkWait, unbounded_wait_never_skips) {
    WorkNode work;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t holder = place_holder(work);
    const uint64_t goal_id = create(work, every_second());
    advance(work, 100U * second);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    EXPECT_EQ(work.goal(goal_id).occurrences_skipped, 0U);
    EXPECT_EQ(work.goal(goal_id).wait_reason, AYNI_REASON_TASK_CAPACITY);

    ASSERT_EQ(work.cancel_task(task_of(work, holder)).status, AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.task(task_of(work, goal_id)).occurrence_count, 100U);
}

// ADR-0020: the wait applies only while the goal may advance, so a paused
// goal's occurrences wait without a deadline; the wait still counts from
// when they fell due, so resuming into a full table skips them at once.
TEST(WorkWait, paused_goal_waits_without_skipping) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.node().config().task_capacity = 1;
    work.run();
    place_holder(work);
    const uint64_t goal_id = create(work, delayed(second));
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    advance(work, 5U * second);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    EXPECT_EQ(work.goal(goal_id).occurrences_skipped, 0U);

    const uint64_t wakes_before = wakes(work);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    EXPECT_EQ(wakes(work), wakes_before + 1U);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(work.goal(goal_id).occurrences_skipped, 1U);
}

// Of several waits that ended, the one that ended first is skipped first.
TEST(WorkWait, earliest_ended_wait_is_skipped_first) {
    WorkNode work;
    work.timer_wait_max_us = second;
    work.node().config().task_capacity = 1;
    work.run();
    place_holder(work);
    const uint64_t later = create(work, delayed(2U * second));
    const uint64_t earlier = create(work, delayed(second, agent_b));
    advance(work, 5U * second);
    ASSERT_EQ(ayni_step(work.kernel(), 1, 1000000, nullptr), AYNI_OK);
    EXPECT_EQ(work.goal(earlier).occurrences_skipped, 1U);
    EXPECT_EQ(work.goal(later).occurrences_skipped, 0U);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.goal(later).occurrences_skipped, 1U);
}

// ADR-0020 decision 10: without room the deadline is the end of the
// earliest bounded wait, not the earliest due occurrence, which here waits
// without limit.
TEST(WorkWait, deadline_is_the_end_of_the_earliest_bounded_wait) {
    WorkNode work;
    work.timer_wait_max_us = second;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t start = now(work);
    place_holder(work);
    GoalSpec unbounded = delayed(second);
    unbounded.type = "reminder";
    const uint64_t waiting = create(work, unbounded);
    const uint64_t bounded = create(work, delayed(2U * second, agent_b));
    EXPECT_EQ(work.node().step().next_deadline_us, start + 3U * second);

    advance_to(work, start + 3U * second);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    EXPECT_EQ(work.goal(bounded).occurrences_skipped, 1U);
    EXPECT_EQ(work.goal(waiting).occurrences_skipped, 0U);
    EXPECT_EQ(work.goal(waiting).wait_reason, AYNI_REASON_TASK_CAPACITY);
}

// wait_reason is read from the tables when queried: what would keep the next
// occurrence from becoming a task now, the task table before the ids, and
// nothing for a goal with no occurrence to come.
TEST(WorkWait, wait_reason_reports_what_would_hold_the_next_occurrence) {
    WorkNode work;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t goal_id = create(work, delayed(10U * second));
    const uint64_t no_occurrence = create(work, expiring(0));
    EXPECT_EQ(work.goal(goal_id).wait_reason, AYNI_REASON_NONE);
    const uint64_t holder = place_holder(work);
    EXPECT_EQ(work.goal(goal_id).wait_reason, AYNI_REASON_TASK_CAPACITY);
    EXPECT_EQ(work.goal(holder).wait_reason, AYNI_REASON_NONE);
    EXPECT_EQ(work.goal(no_occurrence).wait_reason, AYNI_REASON_NONE);
    leave_one_id(work);
    create(work, expiring(0)); // takes the last id
    EXPECT_EQ(work.goal(goal_id).wait_reason, AYNI_REASON_TASK_CAPACITY);
    ASSERT_EQ(work.cancel_task(task_of(work, holder)).status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).wait_reason, AYNI_REASON_ID_EXHAUSTED);
}

} // namespace
