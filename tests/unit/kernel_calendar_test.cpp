// C-TIME-004..007/C-PROD-018: normal calendar behavior through the public C ABI.
#include "work/goal/calendar.h"
#include "work_node.h"
#include <cstring>
#include <gtest/gtest.h>

namespace {
using namespace ayni::test;
using ayni::work::calendar_day_s;
using ayni::work::calendar_days;
constexpr uint32_t calendar_triggers = any_trigger | AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_ONCE) |
                                       AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_DAILY) |
                                       AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_WEEKLY);
int64_t day(int offset = 0) {
    return calendar_days(2026, 10, 27) * calendar_day_s + offset * calendar_day_s;
}
ayni_calendar_rule_v1 rule(ayni_calendar_kind_t kind, int64_t local) {
    ayni_calendar_rule_v1 out{};
    out.struct_size = sizeof(out);
    out.kind = kind;
    out.rule_version = 1;
    out.first_local_s = local;
    constexpr char tz[] = "UTC0";
    out.timezone_size = sizeof(tz) - 1;
    std::memcpy(out.timezone, tz, out.timezone_size);
    return out;
}
uint64_t create(WorkNode &node, const ayni_calendar_rule_v1 &calendar) {
    const auto trigger = calendar.kind == AYNI_CALENDAR_ONCE    ? AYNI_TRIGGER_CALENDAR_ONCE
                         : calendar.kind == AYNI_CALENDAR_DAILY ? AYNI_TRIGGER_CALENDAR_DAILY
                                                                : AYNI_TRIGGER_CALENDAR_WEEKLY;
    auto body = WorkNode::create_body(node.instance(agent_a), 0, "timer", 1, {}, trigger);
    body.calendar = &calendar;
    const auto result = node.create(body);
    EXPECT_EQ(result.status, AYNI_OK) << result.error.reason;
    return result.result.object.id;
}
ayni_calendar_info_v1 info(WorkNode &node, uint64_t goal) {
    ayni_calendar_info_v1 out{};
    out.struct_size = sizeof(out);
    EXPECT_EQ(ayni_get_calendar(node.kernel(), goal, &out), AYNI_OK);
    return out;
}
void settle(WorkNode &node) {
    constexpr uint32_t rounds = 64;
    constexpr uint64_t write_interval_us = 10000;
    for (uint32_t round = 0; round < rounds; ++round) {
        node.node().sim().complete_storage();
        node.node().step();
        node.node().sim().advance_us(write_interval_us);
    }
}

TEST(WorkCalendar, waits_for_trust_then_coalesces_the_latest_missed_occurrence) {
    WorkNode node;
    node.timer_trigger_kinds = calendar_triggers;
    node.run();
    const auto goal = create(node, rule(AYNI_CALENDAR_DAILY, day()));
    settle(node);
    EXPECT_EQ(node.goal(goal).last_task_id, 0U);
    EXPECT_EQ(info(node, goal).waiting_for_time, 1U);
    node.node().sim().set_utc(day(3));
    settle(node);
    const auto id = node.goal(goal).last_task_id;
    ASSERT_NE(id, 0U);
    ayni_task_info_v1 task{};
    task.struct_size = sizeof(task);
    ASSERT_EQ(ayni_get_task(node.kernel(), id, &task), AYNI_OK);
    EXPECT_EQ(task.occurrence_count, 4U);
    EXPECT_EQ(task.calendar_local_s, day(3));
    EXPECT_EQ(task.calendar_utc_s, day(3));
    EXPECT_EQ(info(node, goal).taken, 4U);
    EXPECT_EQ(info(node, goal).waiting_for_time, 0U);
}
TEST(WorkCalendar, backward_clock_correction_never_repeats_a_consumed_day) {
    WorkNode node;
    node.timer_trigger_kinds = calendar_triggers;
    node.run();
    node.node().sim().set_utc(day());
    const auto goal = create(node, rule(AYNI_CALENDAR_DAILY, day()));
    settle(node);
    const auto first = node.goal(goal).last_task_id;
    node.node().sim().set_utc(day(-1));
    settle(node);
    node.node().sim().set_utc(day());
    settle(node);
    EXPECT_EQ(node.goal(goal).last_task_id, first);
    node.node().sim().set_utc(day(1));
    settle(node);
    EXPECT_NE(node.goal(goal).last_task_id, first);
    EXPECT_EQ(info(node, goal).taken, 2U);
}
TEST(WorkCalendar, once_goal_completes_and_utc_correction_preserves_monotonic_delay) {
    WorkNode node;
    node.timer_trigger_kinds = calendar_triggers;
    node.run();
    node.node().sim().set_utc(day());
    const auto goal = create(node, rule(AYNI_CALENDAR_ONCE, day()));
    auto delayed =
        WorkNode::create_body(node.instance(agent_b), 0, "timer", 1, {}, AYNI_TRIGGER_DELAY);
    delayed.trigger_delay_us = 10000000;
    const auto timer = node.create(delayed).result.object.id;
    settle(node);
    EXPECT_EQ(node.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
    node.node().sim().set_utc(day(100));
    settle(node);
    EXPECT_EQ(node.goal(timer).last_task_id, 0U);
    node.node().sim().advance_us(delayed.trigger_delay_us);
    settle(node);
    EXPECT_NE(node.goal(timer).last_task_id, 0U);
}
TEST(WorkCalendar, finite_capacity_wait_does_not_restart_on_each_calendar_poll) {
    WorkNode node;
    node.timer_trigger_kinds = calendar_triggers;
    node.timer_wait_max_us = 2000000;
    node.node().config().task_capacity = 1;
    node.run();
    // A future decision remains live and occupies the only Task slot.
    node.strategy().answer = [](const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &) {
        return AYNI_BUSY;
    };
    auto body =
        WorkNode::create_body(node.instance(agent_b), 0, "reminder", 1, {}, AYNI_TRIGGER_ONCE);
    const auto blocking = node.create(body).result.object.id;
    settle(node);
    ASSERT_NE(node.goal(blocking).last_task_id, 0U);
    node.node().sim().set_utc(day());
    const auto goal = create(node, rule(AYNI_CALENDAR_ONCE, day()));
    settle(node);
    EXPECT_EQ(node.goal(goal).occurrences_skipped, 0U);
    node.node().sim().advance_us(node.timer_wait_max_us);
    settle(node);
    EXPECT_EQ(node.goal(goal).occurrences_skipped, 1U);
    EXPECT_EQ(node.goal(goal).lifecycle, AYNI_GOAL_FAILED);
}
TEST(WorkCalendar, orderly_reopen_preserves_consumed_civil_identity_and_future_rule) {
    WorkNode node;
    node.timer_trigger_kinds = calendar_triggers;
    node.run();
    node.node().sim().set_utc(day());
    const auto goal = create(node, rule(AYNI_CALENDAR_DAILY, day()));
    settle(node);
    const auto task = node.goal(goal).last_task_id;
    ASSERT_NE(task, 0U);
    node.node().reboot();
    node.node().recover();
    node.reopen_session();
    settle(node);
    EXPECT_EQ(node.goal(goal).last_task_id, task);
    EXPECT_EQ(info(node, goal).taken, 1U);
    EXPECT_EQ(node.task(task).calendar_local_s, day());
    node.node().sim().set_utc(day(1));
    settle(node);
    EXPECT_NE(node.goal(goal).last_task_id, task);
    EXPECT_EQ(info(node, goal).taken, 2U);
}
TEST(WorkCalendar, nested_rule_sizes_and_versions_preserve_abi_error_classification) {
    WorkNode node;
    node.timer_trigger_kinds = calendar_triggers;
    node.run();
    auto calendar = rule(AYNI_CALENDAR_ONCE, day());
    auto body = WorkNode::create_body(node.instance(agent_a), 0, "timer", 1, {},
                                      AYNI_TRIGGER_CALENDAR_ONCE);
    body.calendar = &calendar;
    calendar.struct_size = sizeof(calendar) + 1;
    EXPECT_EQ(node.create(body).status, AYNI_UNSUPPORTED_VERSION);
    calendar.struct_size = sizeof(calendar) - 1;
    EXPECT_EQ(node.create(body).status, AYNI_INVALID_ARGUMENT);
    calendar.struct_size = sizeof(calendar);
    calendar.rule_version = 2;
    EXPECT_EQ(node.create(body).status, AYNI_UNSUPPORTED_VERSION);
}
} // namespace
