// ADR-0065: independent generic suspension through the public C ABI.
#include "work_node.h"
#include <gtest/gtest.h>
namespace {
using namespace ayni::test;
constexpr uint32_t autonomous_group = 1;
void settle(WorkNode &node) {
    for (unsigned round = 0; round < 32; ++round) {
        node.node().sim().complete_storage();
        node.node().step();
        node.node().sim().advance_us(10000);
    }
}
Submitted control(WorkNode &node, bool paused, uint64_t command, uint32_t revision) {
    const ayni_pause_group_set_v1 body{sizeof(body), autonomous_group, paused ? 1U : 0U, 0};
    auto intent = node.intent(AYNI_INTENT_PAUSE_GROUP_SET, &body, revision);
    intent.command_id = command;
    return node.submit(intent);
}
ayni_pause_group_info_v1 info(WorkNode &node) {
    ayni_pause_group_info_v1 out{};
    out.struct_size = sizeof(out);
    EXPECT_EQ(ayni_get_pause_group(node.kernel(), autonomous_group, &out), AYNI_OK);
    return out;
}
uint64_t create(WorkNode &node, uint32_t group, uint64_t owner = agent_a, uint64_t parent = 0,
                ayni_trigger_kind_t trigger = AYNI_TRIGGER_ONCE) {
    auto body = WorkNode::create_body(node.instance(owner), parent, "timer", 1, {}, trigger);
    body.pause_group = group;
    const auto result = node.create(body);
    EXPECT_EQ(result.status, AYNI_OK);
    return result.result.object.id;
}
// Existing and newly created autonomous work are gated; explicit work still advances.
TEST(PauseGroup, pause_preserves_reminders_and_applies_to_new_autonomous_work) {
    WorkNode node;
    node.run();
    const auto autonomous = create(node, autonomous_group);
    node.node().step(1);
    const auto task = node.goal(autonomous).last_task_id;
    ASSERT_NE(task, 0U);
    ASSERT_EQ(control(node, true, 1, 0).status, AYNI_OK);
    EXPECT_EQ(node.task(task).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(node.goal(autonomous).lifecycle, AYNI_GOAL_ACTIVE);
    const auto explicit_work = create(node, 0, agent_b);
    const auto newer = create(node, autonomous_group, agent_b);
    settle(node);
    EXPECT_EQ(node.goal(explicit_work).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(node.goal(newer).last_task_id, 0U);
    EXPECT_EQ(node.goal(newer).group_paused, 1U);
    ASSERT_EQ(control(node, false, 2, 1).status, AYNI_OK);
    settle(node);
    EXPECT_EQ(node.goal(autonomous).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(node.goal(newer).lifecycle, AYNI_GOAL_SATISFIED);
}
// Group reopening cannot erase a separate per-Goal user pause or ancestor gate.
TEST(PauseGroup, ancestor_membership_and_user_pause_remain_independent) {
    WorkNode node;
    node.run();
    const auto parent = create(node, autonomous_group, agent_a, 0, AYNI_TRIGGER_NONE);
    ASSERT_EQ(control(node, true, 1, 0).status, AYNI_OK);
    const auto child = create(node, 2, agent_a, parent);
    settle(node);
    EXPECT_EQ(node.goal(child).last_task_id, 0U);
    ASSERT_EQ(node.change(AYNI_INTENT_GOAL_PAUSE, child).status, AYNI_OK);
    ASSERT_EQ(control(node, false, 2, 1).status, AYNI_OK);
    settle(node);
    EXPECT_EQ(node.goal(child).lifecycle, AYNI_GOAL_PAUSED);
    ASSERT_EQ(node.change(AYNI_INTENT_GOAL_RESUME, child).status, AYNI_OK);
    settle(node);
    EXPECT_EQ(node.goal(child).lifecycle, AYNI_GOAL_SATISFIED);
}
// Only future periodic work is derived after an explicit group reopening.
TEST(PauseGroup, reopening_skips_periodic_autonomous_backlog) {
    WorkNode node;
    node.run();
    auto body =
        WorkNode::create_body(node.instance(agent_a), 0, "timer", 1, {}, AYNI_TRIGGER_INTERVAL);
    body.pause_group = autonomous_group;
    body.trigger_delay_us = AYNI_TRIGGER_PERIOD_MIN_US;
    body.trigger_period_us = AYNI_TRIGGER_PERIOD_MIN_US;
    const auto created = node.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    ASSERT_EQ(control(node, true, 1, 0).status, AYNI_OK);
    node.node().sim().advance_us(5 * body.trigger_period_us);
    ASSERT_EQ(control(node, false, 2, 1).status, AYNI_OK);
    EXPECT_EQ(node.goal(created.result.object.id).last_task_id, 0U);
    EXPECT_EQ(node.goal(created.result.object.id).occurrences_skipped, 5U);
    settle(node);
    EXPECT_EQ(node.goal(created.result.object.id).last_task_id, 0U);
    node.node().sim().advance_us(body.trigger_period_us);
    settle(node);
    EXPECT_NE(node.goal(created.result.object.id).last_task_id, 0U);
}
// Queued input from the paused interval cannot become new work after reopen.
TEST(PauseGroup, reopening_ignores_preceding_queued_inputs_but_accepts_new_input) {
    WorkNode node;
    node.run();
    auto body =
        WorkNode::create_body(node.instance(agent_a), 0, "timer", 1, {}, AYNI_TRIGGER_LOCAL_INPUT);
    body.pause_group = autonomous_group;
    body.trigger_source = text("button");
    const auto created = node.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    ASSERT_EQ(control(node, true, 1, 0).status, AYNI_OK);
    ayni_event_v1 event{};
    event.struct_size = sizeof(event);
    event.kind = AYNI_EVENT_LOCAL_INPUT;
    event.source = text("button");
    ASSERT_EQ(ayni_post_event(node.kernel(), &event), AYNI_OK);
    ASSERT_EQ(control(node, false, 2, 1).status, AYNI_OK);
    settle(node);
    EXPECT_EQ(node.goal(created.result.object.id).last_task_id, 0U);
    ASSERT_EQ(ayni_post_event(node.kernel(), &event), AYNI_OK);
    settle(node);
    EXPECT_NE(node.goal(created.result.object.id).last_task_id, 0U);
}

// ADR-0065/C-AUTH-027: pending approval never accumulates or skips occurrences.
TEST(PauseGroup, pending_calendar_survives_group_reopen_approval_and_normal_reopen) {
    WorkNode node;
    node.node().config().goal_type_capacity = 4;
    node.additional_registration = [](ayni_kernel_t *kernel) {
        const ayni_capability_desc_v1 cap{sizeof(cap), 1, text("calendar.test"), 0, 0, nullptr};
        ASSERT_EQ(ayni_register_capability(kernel, &cap), AYNI_OK);
        auto type = goal_type_desc("calendar.pending", 1, nullptr, nullptr,
                                   AYNI_TRIGGER_BIT(AYNI_TRIGGER_CALENDAR_DAILY));
        type.export_requirements = [](void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
            const ayni_goal_requirement_v1 need{
                sizeof(need), 1, text("calendar.test"), {sizeof(ayni_scope_v1), 0, nullptr}};
            return sink->emit(sink->ctx, &need);
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
    };
    node.run();
    node.node().sim().set_utc(10000);
    ayni_calendar_rule_v1 rule{};
    rule.struct_size = sizeof(rule);
    rule.kind = AYNI_CALENDAR_DAILY;
    rule.rule_version = 1;
    rule.first_local_s = 10000;
    rule.timezone_size = 4;
    std::memcpy(rule.timezone, "UTC0", 4);
    auto body = WorkNode::create_body(node.instance(agent_a), 0, "calendar.pending", 1, {},
                                      AYNI_TRIGGER_CALENDAR_DAILY);
    body.calendar = &rule;
    body.pause_group = autonomous_group;
    const auto created = node.create(body);
    ASSERT_EQ(created.status, AYNI_OK);
    ASSERT_EQ(node.goal(created.result.object.id).lifecycle, AYNI_GOAL_PROPOSED);
    ASSERT_EQ(control(node, true, 1, 0).status, AYNI_OK);
    node.node().sim().set_utc(10000 + 3 * 86400);
    settle(node);
    ASSERT_EQ(control(node, false, 2, 1).status, AYNI_OK);
    EXPECT_EQ(node.goal(created.result.object.id).occurrences, 0U);
    EXPECT_EQ(node.goal(created.result.object.id).occurrences_skipped, 0U);
    ayni_goal_approve_v1 approve{};
    approve.struct_size = sizeof(approve);
    approve.goal_id = created.result.object.id;
    approve.missing_mask = 1;
    approve.lifetime = AYNI_GRANT_PERMANENT;
    approve.original = body;
    auto intent =
        node.intent(AYNI_INTENT_GOAL_APPROVE, &approve, node.goal(approve.goal_id).revision);
    intent.command_id = 3;
    ASSERT_EQ(node.submit(intent).status, AYNI_OK);
    settle(node);
    node.node().reboot();
    node.node().recover();
    EXPECT_EQ(node.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
}

// Exact command retries preserve their first acceptance across later preference changes.
TEST(PauseGroup, retries_keep_the_original_receipt_and_complete_request_identity) {
    WorkNode node;
    node.run();
    const auto paused = control(node, true, 1, 0);
    ASSERT_EQ(paused.status, AYNI_OK);
    ASSERT_EQ(control(node, false, 2, 1).status, AYNI_OK);
    const auto retried = control(node, true, 1, 0);
    ASSERT_EQ(retried.status, AYNI_OK);
    EXPECT_EQ(retried.result.commit_seq, paused.result.commit_seq);
    EXPECT_EQ(info(node).paused, 0U);
    EXPECT_EQ(info(node).revision, 2U);
    EXPECT_EQ(control(node, false, 1, 0).status, AYNI_IDEMPOTENCY_CONFLICT);
}
// Normal graceful close/reopen retains both the preference and immutable membership.
TEST(PauseGroup, normal_close_reopen_preserves_the_gate_and_new_goal_membership) {
    WorkNode node;
    node.run();
    ASSERT_EQ(control(node, true, 1, 0).status, AYNI_OK);
    const auto goal = create(node, autonomous_group);
    settle(node);
    node.node().reboot();
    node.node().recover();
    node.reopen_session();
    EXPECT_EQ(info(node).paused, 1U);
    EXPECT_EQ(info(node).revision, 1U);
    EXPECT_EQ(node.goal(goal).pause_group, autonomous_group);
    settle(node);
    EXPECT_EQ(node.goal(goal).last_task_id, 0U);
    ASSERT_EQ(control(node, false, 2, 1).status, AYNI_OK);
    settle(node);
    EXPECT_EQ(node.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
}
} // namespace
