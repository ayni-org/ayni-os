#include "motor_check.h"
#include <ayni/values.hpp>
#include <gtest/gtest.h>
#include <vector>
namespace {
using namespace wifichat;
struct Owner {
    bool enabled = true, accepts_notice = true;
    unsigned submits = 0, stops = 0;
    ayni_status_t submit_status = AYNI_DENIED;
    ayni_reason_t submit_reason = AYNI_REASON_GOAL_AUTHORITY_MISSING;
    std::vector<MotorCheckNotice> notices;
    static bool ready(const void *p) { return static_cast<const Owner *>(p)->enabled; }
    static MotorCheck::SubmitResult submit(void *p, ayni_intent_kind_t kind, const void *,
                                           uint64_t *, uint32_t) {
        auto &self = *static_cast<Owner *>(p);
        ++self.submits;
        EXPECT_EQ(kind, AYNI_INTENT_GOAL_CREATE);
        return {self.submit_status, self.submit_reason};
    }
    static bool notify(void *p, uint64_t, const MotorCheckNotice &notice) {
        auto &self = *static_cast<Owner *>(p);
        if (!self.accepts_notice) {
            return false;
        }
        self.notices.push_back(notice);
        return true;
    }
    static void stop(void *p) { ++static_cast<Owner *>(p)->stops; }
    MotorCheck::Owner port() { return {this, ready, submit, notify, stop}; }
};
TEST(MotorCheck, PublicRulesRequestsOneFixedMotorThroughTheNormalCallBoundary) {
    // ADR-0097: local checks reuse SDK Rules and motor.run, not a second driver or model loop.
    Owner owner;
    MotorCheck check;
    ASSERT_EQ(check.configure(owner.port()), AYNI_OK);
    auto strategy = check.strategy();
    struct Capture {
        unsigned count = 0;
        int64_t first = 0, second = 0, duration = 0;
        static ayni_status_t request(void *p, const ayni_call_request_v1 *call) {
            auto &self = *static_cast<Capture *>(p);
            ++self.count;
            EXPECT_EQ(std::string_view(call->binding_id.data, call->binding_id.size), "motor.run");
            EXPECT_EQ(call->binding_version, motor_version);
            EXPECT_EQ(call->mode, AYNI_CALL_MODE_FOREGROUND);
            EXPECT_EQ(call->preemption, AYNI_CALL_PREEMPT_SUPERSEDE);
            EXPECT_EQ(call->value.node_count, 6U);
            auto nodes = motor_run_schema();
            const ayni_schema_v1 schema{nodes.data(), uint32_t(nodes.size())};
            size_t bytes = 0, alignment = 0;
            EXPECT_EQ(ayni_value_workspace_requirements(&bytes, &alignment), AYNI_OK);
            std::vector<uint64_t> region((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
            const ayni_workspace_v1 workspace{sizeof(workspace), 0, region.data(),
                                              region.size() * sizeof(uint64_t)};
            std::array<ayni_value_node_v1, 6> normalized_nodes{};
            std::array<char, motor_input_bytes_max> normalized_text{};
            ayni_value_buffer_v1 normalized{
                normalized_nodes.data(), uint32_t(normalized_nodes.size()), 0,
                normalized_text.data(),  uint32_t(normalized_text.size()),  0};
            EXPECT_EQ(ayni_value_normalize(&workspace, &schema, &call->value, &normalized),
                      AYNI_OK);
            EXPECT_EQ(normalized.node_count, 6U);
            EXPECT_EQ(normalized.nodes[1].kind, AYNI_VALUE_LIST);
            EXPECT_EQ(normalized.nodes[1].field_id, 1U);
            EXPECT_EQ(normalized.nodes[2].kind, AYNI_VALUE_OBJECT);
            self.first = normalized.nodes[3].value.integer;
            self.second = normalized.nodes[4].value.integer;
            self.duration = normalized.nodes[5].value.integer;
            return AYNI_OK;
        }
        static ayni_status_t result(const void *, ayni_string_v1, ayni_task_call_result_v1 *) {
            return AYNI_NOT_FOUND;
        }
    } capture;
    for (uint32_t motor = 0; motor < MotorCheck::type_count; ++motor) {
        auto task = ayni_task_snapshot_v1{};
        task.struct_size = sizeof(task);
        task.task_id = 1;
        task.goal_type_id = motor_text(motor == 0 ? "wifi.motor.check1" : "wifi.motor.check2");
        task.goal_type_version = 1;
        task.step_capacity = 1;
        task.call_result = Capture::result;
        ayni_policy_intent_v1 intent{};
        ayni_policy_output_v1 out{};
        out.struct_size = sizeof(out);
        out.intent_capacity = 1;
        out.intents = &intent;
        out.request_call = Capture::request;
        out.call_context = &capture;
        ASSERT_EQ(strategy.decide(strategy.ctx, &task, &out), AYNI_OK);
        EXPECT_EQ(capture.first, motor == 0 ? MotorCheck::percent : 0);
        EXPECT_EQ(capture.second, motor == 1 ? MotorCheck::percent : 0);
        EXPECT_EQ(capture.duration, MotorCheck::duration_ms);
    }
    EXPECT_EQ(capture.count, MotorCheck::type_count);
}
TEST(MotorCheck, OwnerReadinessGatesThePolicyAndRejectedRequestsRemainVisible) {
    // ADR-0097: boot/session preparation gates all checks; no startup movement is admitted.
    Owner owner;
    owner.enabled = false;
    MotorCheck check;
    ASSERT_EQ(check.configure(owner.port()), AYNI_OK);
    const auto strategy = check.strategy();
    EXPECT_EQ(strategy.ready(strategy.ctx, 1), 0U);
    ASSERT_EQ(check.request(0, 1), AYNI_OK);
    ASSERT_EQ(owner.notices.size(), 1U);
    EXPECT_EQ(owner.notices.back().status, AYNI_NOT_READY);
    EXPECT_EQ(owner.submits, 0U);
    EXPECT_FALSE(check.active());
    owner.enabled = true;
    EXPECT_EQ(strategy.ready(strategy.ctx, 1), 1U);
    EXPECT_EQ(check.request(MotorCheck::type_count, 2), AYNI_INVALID_ARGUMENT);
}
TEST(MotorCheck, RegisteredGoalRequiresOnlyTheExactFixedProcedure) {
    // ADR-0097: local diagnostics retain Grant admission, with a singleton test ceiling.
    Owner owner;
    MotorCheck check;
    ASSERT_EQ(check.configure(owner.port()), AYNI_OK);
    struct Capture {
        uint32_t motor = 0, calls = 0;
        static ayni_status_t emit(void *context, const ayni_goal_requirement_v1 *need) {
            auto &self = *static_cast<Capture *>(context);
            ++self.calls;
            EXPECT_EQ(std::string_view(need->capability_id.data, need->capability_id.size),
                      "motor.run");
            EXPECT_EQ(need->capability_version, motor_version);
            EXPECT_EQ(need->scope.field_count, 3U);
            for (uint32_t field = 0; field < need->scope.field_count; ++field) {
                const auto &value = need->scope.fields[field];
                const int64_t expected = field == 2            ? MotorCheck::duration_ms
                                         : field == self.motor ? MotorCheck::percent
                                                               : 0;
                EXPECT_EQ(value.field_id, field + 1);
                EXPECT_EQ(value.allowed.kind, AYNI_AUTHORITY_I64);
                EXPECT_EQ(value.allowed.values.integer.min_inclusive, expected);
                EXPECT_EQ(value.allowed.values.integer.max_inclusive, expected);
            }
            return AYNI_OK;
        }
    } capture;
    const ayni_goal_requirement_sink_v1 sink{sizeof(sink), &capture, Capture::emit};
    for (uint32_t motor = 0; motor < MotorCheck::type_count; ++motor) {
        capture.motor = motor;
        ayni_goal_type_v1 type{};
        ASSERT_EQ(check.describe_type(motor, type), AYNI_OK);
        EXPECT_EQ(type.trigger_kinds, AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE));
        ASSERT_EQ(type.validate(type.ctx, {}), AYNI_OK);
        ASSERT_EQ(type.export_requirements(type.ctx, {}, &sink), AYNI_OK);
    }
    EXPECT_EQ(capture.calls, MotorCheck::type_count);
}
TEST(MotorCheck, TerminalQueueBackpressureDoesNotResubmitTheGoal) {
    // ADR-0097: retained terminal delivery never repeats a refused physical workflow.
    Owner owner;
    owner.accepts_notice = false;
    MotorCheck check;
    ASSERT_EQ(check.configure(owner.port()), AYNI_OK);
    ASSERT_EQ(check.request(0, 1), AYNI_OK);
    check.tick(nullptr, 1, 0);
    EXPECT_EQ(owner.submits, 1U);
    EXPECT_TRUE(check.active());
    check.tick(nullptr, 1, 0);
    EXPECT_EQ(owner.submits, 1U);
    owner.accepts_notice = true;
    check.tick(nullptr, 1, 0);
    EXPECT_FALSE(check.active());
    ASSERT_FALSE(owner.notices.empty());
    EXPECT_EQ(owner.notices.back().phase, MotorCheckNotice::Phase::failed);
    EXPECT_EQ(owner.notices.back().status, AYNI_DENIED);
    EXPECT_EQ(owner.notices.back().reason, owner.submit_reason);
}
TEST(MotorCheck, CancellationBeforeCreationStopsWithoutSubmittingAnyGoal) {
    // ADR-0097: local stop precedes Core management and does not depend on queue room.
    Owner owner;
    MotorCheck check;
    ASSERT_EQ(check.configure(owner.port()), AYNI_OK);
    ASSERT_EQ(check.request(1, 1), AYNI_OK);
    check.cancel();
    EXPECT_EQ(owner.stops, 1U);
    EXPECT_EQ(owner.submits, 0U);
    EXPECT_FALSE(check.active());
    EXPECT_EQ(owner.notices.back().phase, MotorCheckNotice::Phase::cancelled);
}
TEST(MotorCheck, NoticePreservesErrorCodesAndOnlyKnownWireStatesAreAccepted) {
    // ADR-0097: touchscreen reports actual classified codes without model/voice content.
    MotorCheckNotice before{MotorCheckNotice::Phase::failed, 1, AYNI_DENIED,
                            AYNI_REASON_EXECUTION_SCENE};
    std::array<uint8_t, MotorCheckNotice::wire_bytes> bytes{};
    before.encode(bytes.data());
    MotorCheckNotice after;
    ASSERT_TRUE(MotorCheckNotice::decode(bytes.data(), bytes.size(), after));
    EXPECT_EQ(after.status, before.status);
    EXPECT_EQ(after.reason, before.reason);
    EXPECT_EQ(after.motor, 1U);
    put32(bytes.data(), 0);
    EXPECT_FALSE(MotorCheckNotice::decode(bytes.data(), bytes.size(), after));
    EXPECT_TRUE(MotorCheck::owns(motor_text("wifi.motor.check1"), 1));
    EXPECT_FALSE(MotorCheck::owns(motor_text("usb.chat"), 1));
}
TEST(MotorCheck, ValidMotorTrajectoryEnforcesSegmentAndTotalDurationBounds) {
    // ADR-0112: v3 motor.run supports 1..8 segments including intermediate (0, 0) coast pauses,
    // rejects empty/all-zero/overlong trajectories, and caps combined duration at 60 s.
    MotorTrajectory trajectory{};
    EXPECT_FALSE(valid_motor_trajectory(trajectory));

    trajectory.count = 3;
    trajectory.segments[0] = {45, 45, 500};
    trajectory.segments[1] = {0, 0, 200};
    trajectory.segments[2] = {45, -45, 400};
    EXPECT_TRUE(valid_motor_trajectory(trajectory));
    EXPECT_EQ(total_duration_ms(trajectory), 1100U);

    MotorTrajectory all_zero{};
    all_zero.count = 2;
    all_zero.segments[0] = {0, 0, 100};
    all_zero.segments[1] = {0, 0, 100};
    EXPECT_FALSE(valid_motor_trajectory(all_zero));

    MotorTrajectory over_duration{};
    over_duration.count = 2;
    over_duration.segments[0] = {30, 30, 35000};
    over_duration.segments[1] = {30, 30, 30000};
    EXPECT_FALSE(valid_motor_trajectory(over_duration));

    MotorTrajectory too_many{};
    too_many.count = motor_segments_max + 1;
    EXPECT_FALSE(valid_motor_trajectory(too_many));
}
} // namespace
