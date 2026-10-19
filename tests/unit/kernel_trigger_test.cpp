// Time triggers through the C ABI (ADR-0019): delay and interval goals on
// the host's monotonic clock, occurrences that wait becoming one task, the
// order in which due goals derive, and the deadline ayni_step reports. Runs
// on the simulation Host, whose clock moves only when a test moves it.

#include "kernel/kernel_state.h"
#include "kernel_node.h"
#include "work_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using namespace ayni::test;

constexpr uint64_t no_time = std::numeric_limits<uint64_t>::max();
constexpr uint64_t second = AYNI_TRIGGER_PERIOD_MIN_US;

ayni_goal_create_v1 timed_body(WorkNode &work, ayni_trigger_kind_t kind, uint64_t delay_us,
                               uint64_t period_us = 0, uint64_t agent_id = agent_a,
                               uint64_t parent_goal_id = 0) {
    ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_id), parent_goal_id,
                                                     "timer", 1, {nullptr, 0}, kind);
    body.trigger_delay_us = delay_us;
    body.trigger_period_us = period_us;
    return body;
}

uint64_t create(WorkNode &work, const ayni_goal_create_v1 &body) {
    const Submitted created = work.create(body);
    EXPECT_EQ(created.status, AYNI_OK);
    return created.result.object.id;
}

uint64_t delay_goal(WorkNode &work, uint64_t delay_us, uint64_t agent_id = agent_a) {
    return create(work, timed_body(work, AYNI_TRIGGER_DELAY, delay_us, 0, agent_id));
}

uint64_t interval_goal(WorkNode &work, uint64_t delay_us, uint64_t period_us,
                       uint64_t agent_id = agent_a) {
    return create(work, timed_body(work, AYNI_TRIGGER_INTERVAL, delay_us, period_us, agent_id));
}

uint64_t now(WorkNode &work) {
    return work.node().sim().now_us();
}

void advance(WorkNode &work, uint64_t delta_us) {
    work.node().sim().advance_us(delta_us);
}

uint64_t task_of(WorkNode &work, uint64_t goal_id) {
    return work.goal(goal_id).last_task_id;
}

// Moves the clock to time_us.
void advance_to(WorkNode &work, uint64_t time_us) {
    ASSERT_GE(time_us, now(work));
    advance(work, time_us - now(work));
}

// --- Delay -----------------------------------------------------------------

// ADR-0019: the occurrence falls due delay_us after creation, and the step
// before it reports that time as its deadline.
TEST(WorkTrigger, delay_goal_derives_once_its_time_comes) {
    WorkNode work;
    work.run();
    advance(work, 1000);
    const uint64_t created_at = now(work);
    const uint64_t wakes_before = work.node().sim().wake_count();
    const uint64_t goal_id = delay_goal(work, 5U * second);
    // The owner steps to learn the new deadline.
    EXPECT_EQ(work.node().sim().wake_count(), wakes_before + 1U);
    const uint64_t due_at = created_at + 5U * second;
    ayni_goal_info_v1 goal = work.goal(goal_id);
    EXPECT_EQ(goal.trigger, AYNI_TRIGGER_DELAY);
    EXPECT_EQ(goal.trigger_delay_us, 5U * second);
    EXPECT_EQ(goal.trigger_period_us, 0U);
    EXPECT_EQ(goal.next_due_us, due_at);

    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(report.next_deadline_us, due_at);
    advance_to(work, due_at - 1U);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, due_at);

    advance(work, 1);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    goal = work.goal(goal_id);
    EXPECT_EQ(goal.occurrences, 1U);
    EXPECT_EQ(goal.next_due_us, no_time);
    const ayni_task_info_v1 task = work.task(goal.last_task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(task.occurrence, 1U);
    EXPECT_EQ(task.occurrence_count, 1U);
    EXPECT_EQ(task.due_us, due_at);
}

TEST(WorkTrigger, delay_past_the_clock_never_falls_due) {
    WorkNode work;
    work.run();
    advance(work, 10);
    const uint64_t goal_id = delay_goal(work, no_time);
    EXPECT_EQ(work.goal(goal_id).next_due_us, no_time);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
}

// --- Interval --------------------------------------------------------------

TEST(WorkTrigger, interval_goal_derives_each_occurrence_on_its_schedule) {
    WorkNode work;
    work.run();
    const uint64_t start = now(work);
    const uint64_t goal_id = interval_goal(work, 2U * second, 3U * second);
    EXPECT_EQ(work.goal(goal_id).trigger_period_us, 3U * second);
    EXPECT_EQ(work.node().step().next_deadline_us, start + 2U * second);

    uint64_t previous_task = 0;
    for (uint32_t number = 1; number <= 3; ++number) {
        const uint64_t due_at = start + 2U * second + (number - 1U) * 3U * second;
        // A little late, as a host that wakes after its deadline is.
        advance_to(work, due_at + 40U);
        const ayni_step_report_v1 report = work.node().step();
        EXPECT_EQ(report.items_done, 1U) << number;
        EXPECT_EQ(report.next_deadline_us, due_at + 3U * second) << number;
        const ayni_task_info_v1 task = work.task(task_of(work, goal_id));
        EXPECT_GT(task.task_id, previous_task);
        EXPECT_EQ(task.occurrence, number);
        EXPECT_EQ(task.occurrence_count, 1U);
        EXPECT_EQ(task.due_us, due_at);
        previous_task = task.task_id;
    }
    EXPECT_EQ(work.goal(goal_id).occurrences, 3U);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
}

// C-PROD-018, C-LIMIT-007: occurrences that fell due before a step ran
// become one task that names the latest and how many it stands for; the
// next occurrence keeps the original schedule.
TEST(WorkTrigger, occurrences_missed_between_steps_become_one_task) {
    WorkNode work;
    work.run();
    const uint64_t start = now(work);
    const uint64_t goal_id = interval_goal(work, 0, second);
    advance(work, 3U * second + second / 2U);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(report.next_deadline_us, start + 4U * second);
    const ayni_task_info_v1 task = work.task(task_of(work, goal_id));
    EXPECT_EQ(task.occurrence, 4U);
    EXPECT_EQ(task.occurrence_count, 4U);
    EXPECT_EQ(task.due_us, start + 3U * second);
    EXPECT_EQ(work.goal(goal_id).next_due_us, start + 4U * second);
}

// --- Goals that may not advance ----------------------------------------------

// ADR-0019: the clock keeps running while a goal is paused; what fell due
// meanwhile waits and becomes one task when the goal is resumed, and the
// held goal sets no deadline, so the host does not wake for it.
TEST(WorkTrigger, paused_goal_takes_what_fell_due_when_resumed) {
    WorkNode work;
    work.run();
    const uint64_t start = now(work);
    const uint64_t goal_id = interval_goal(work, second, 2U * second);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).next_due_us, start + second);
    advance(work, 6U * second);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);

    const uint64_t wakes_before = work.node().sim().wake_count();
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    EXPECT_EQ(work.node().sim().wake_count(), wakes_before + 1U);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, start + 7U * second);
    const ayni_task_info_v1 task = work.task(task_of(work, goal_id));
    EXPECT_EQ(task.occurrence, 3U); // at 1, 3 and 5 seconds
    EXPECT_EQ(task.occurrence_count, 3U);
    EXPECT_EQ(task.due_us, start + 5U * second);
}

TEST(WorkTrigger, paused_instance_holds_the_occurrence_until_resumed) {
    WorkNode work;
    work.run();
    const uint64_t start = now(work);
    const uint64_t goal_id = delay_goal(work, second);
    EXPECT_EQ(work.node().step().next_deadline_us, start + second);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
    EXPECT_EQ(work.node().step().next_deadline_us, no_time);
    advance(work, 2U * second);
    EXPECT_EQ(work.node().step().items_done, 0U);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_a).status, AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.task(task_of(work, goal_id)).due_us, start + second);
}

TEST(WorkTrigger, cancelled_goal_has_no_occurrence_to_come) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = interval_goal(work, second, second);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, goal_id).status, AYNI_OK);
    EXPECT_EQ(work.goal(goal_id).next_due_us, no_time);
    advance(work, 5U * second);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    EXPECT_EQ(task_of(work, goal_id), 0U);
}

// --- Order and deadlines -----------------------------------------------------

// ADR-0019: of the goals with an occurrence due, the one due earliest
// derives first; equal times go in creation order.
TEST(WorkTrigger, goal_due_earliest_derives_first) {
    WorkNode work;
    work.run();
    const uint64_t late = delay_goal(work, 3U * second);
    const uint64_t early = delay_goal(work, second);
    const uint64_t tie_first = delay_goal(work, 2U * second);
    const uint64_t tie_second = delay_goal(work, 2U * second);
    advance(work, 4U * second);
    for (const uint64_t expected : {early, tie_first, tie_second, late}) {
        ASSERT_EQ(work.node().step(1).items_done, 1U);
        EXPECT_NE(task_of(work, expected), 0U) << expected;
    }
    EXPECT_LT(task_of(work, early), task_of(work, tie_first));
    EXPECT_LT(task_of(work, tie_first), task_of(work, tie_second));
    EXPECT_LT(task_of(work, tie_second), task_of(work, late));
}

// The deadline is the earliest occurrence of a goal that may advance; a full
// task table sets none, since only a task that ends makes room, and the
// intent that ends it wakes the owner. Pausing the goal due earliest wakes
// no one, but the next step reports the deadline without it.
TEST(WorkTrigger, deadline_counts_only_goals_that_could_derive) {
    WorkNode work;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t start = now(work);
    const uint64_t held = delay_goal(work, second);
    const uint64_t later = delay_goal(work, 3U * second, agent_b);
    EXPECT_EQ(work.node().step().next_deadline_us, start + second);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, held).status, AYNI_OK);
    EXPECT_EQ(work.node().step().next_deadline_us, start + 3U * second);

    const uint64_t first = create(work, timed_body(work, AYNI_TRIGGER_ONCE, 0, 0, agent_b));
    EXPECT_EQ(work.node().step().items_done, 1U); // takes the only place
    advance(work, 4U * second);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);

    const uint64_t wakes_before = work.node().sim().wake_count();
    ASSERT_EQ(work.cancel_task(task_of(work, first)).status, AYNI_OK);
    EXPECT_EQ(work.node().sim().wake_count(), wakes_before + 1U);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(work.task(task_of(work, later)).due_us, start + 3U * second);
}

// lifecycles.md: only RUNNING derives, so no other state reports a
// deadline for an occurrence.
TEST(WorkTrigger, draining_reports_no_deadline) {
    WorkNode work;
    work.run();
    delay_goal(work, second);
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
}

// A step takes occurrences as of the time it started, so a clock that moves
// during the step does not split one schedule across two times.
TEST(WorkTrigger, step_takes_occurrences_as_of_its_start) {
    WorkNode work;
    work.run();
    const uint64_t start = now(work);
    const uint64_t goal_id = interval_goal(work, 0, second);
    advance(work, second - 1U);
    work.node().sim().advance_per_read_us(10);
    const ayni_step_report_v1 report = work.node().step();
    work.node().sim().advance_per_read_us(0);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(work.task(task_of(work, goal_id)).occurrence_count, 1U);
    EXPECT_EQ(report.next_deadline_us, start + second);
}

} // namespace
