// ADR-0114: exercise production catalog and Provider handoff without a physical executor.
#include "motor.h"
#include "work_node.h"
#include <ayni/workspace.hpp>
#include <cstdio>
#include <esp_timer.h>
#include <gtest/gtest.h>

namespace wifichat {
// This fixture never activates an operation. Hardware paths explicitly refuse if reached.
ayni_executor_v1 MotorExecution::ports() {
    ayni_executor_v1 out{};
    out.struct_size = sizeof(out);
    out.version = motor_version;
    out.executor_id = motor_text("motor.executor");
    out.control_domain_id = motor_domain;
    out.executor_boot_epoch = out.safety_epoch = 1;
    out.calibration_version = out.control_mode = 1;
    out.procedure_max_us = motor_procedure_us;
    out.session_lease_max_us = motor_session_lease_us;
    out.command_age_max_us = motor_sample_age_us;
    out.poll_period_us = motor_poll_us;
    out.challenge = [](void *, ayni_attempt_ref_v1, const ayni_permit_epochs_v1 *,
                       ayni_executor_completion_t *) { return AYNI_DENIED; };
    out.renew = [](void *, ayni_attempt_ref_v1, const ayni_permit_renewal_v1 *,
                   ayni_executor_completion_t *) { return AYNI_DENIED; };
    out.stop = out.status = [](void *, ayni_attempt_ref_v1, uint64_t,
                               ayni_executor_completion_t *) { return AYNI_DENIED; };
    return out;
}
ayni_submit_result_t MotorExecution::submit(const ayni_provider_request_v1 *,
                                            ayni_provider_completion_t *) {
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t MotorExecution::submit_soft_stop(const ayni_provider_request_v1 *,
                                                      ayni_provider_completion_t *) {
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t MotorExecution::submit_stream(ayni_attempt_ref_v1, uint64_t,
                                                   ayni_provider_completion_t *,
                                                   const ayni_execution_permit_v1 &,
                                                   const MotorCommand &, uint64_t, uint64_t,
                                                   uint64_t) {
    ADD_FAILURE() << "closing/opening fixture must never dispatch physical output";
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t MotorExecution::cancel(ayni_attempt_ref_v1) {
    return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
int MotorExecution::describe(char *out, size_t bytes) const {
    return std::snprintf(out, bytes, "{}");
}
} // namespace wifichat

TEST(WifiMotorSessions, ProductionRegistrationAndShutdownDrainRetainedOpening) {
    using namespace ayni::test;
    using namespace wifichat;
    WorkNode work;
    ControlMailbox mailbox;
    Transport transport(mailbox);
    Motor motor(transport);
    ayni::Workspace workspace;
    ASSERT_EQ(workspace.create(ayni_value_workspace_requirements), AYNI_OK);
    auto &config = work.node().config();
    config.capability_capacity = config.call_capability_capacity = config.binding_capacity = 12;
    config.provider_capacity = 4;
    config.strategy_capacity = 4;
    config.goal_type_capacity = 6;
    config.goal_parameter_max_bytes = 1024;
    config.call_capacity = config.resource_request_capacity = 8;
    config.resource_capacity = 4;
    config.executor_capacity = 2;
    config.execution_binding_capacity = 2;
    config.scene_capacity = 2;
    config.call_input_max_bytes = motor_input_bytes_max;
    config.call_result_max_bytes = motor_reply_bytes_max;
    uint64_t command = 100;
    work.additional_registration = [&](ayni_kernel_t *kernel) {
        ASSERT_EQ(motor.configure(kernel), AYNI_OK);
        ayni::runtime::OperationSession::Commands commands;
        commands.context = &work;
        commands.submit = [](void *, ayni_intent_kind_t, const void *, uint64_t *, uint32_t) {
            return ayni::runtime::OperationSession::SubmitResult{AYNI_DENIED, AYNI_REASON_NONE};
        };
        ASSERT_EQ(motor.configure_session(kernel, workspace.get(), commands), AYNI_OK);
        auto type = goal_type_desc("motor.test", 1);
        type.export_requirements = [](void *, ayni_bytes_v1,
                                      const ayni_goal_requirement_sink_v1 *sink) {
            const ayni_goal_requirement_v1 need{sizeof(need),
                                                motor_version,
                                                text("motor.stream"),
                                                {sizeof(ayni_scope_v1), 0, nullptr}};
            return sink->emit(sink->ctx, &need);
        };
        type.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT, 0};
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
    };
    work.run();
    const ayni_grant_create_v1 grant{sizeof(grant),
                                     motor_version,
                                     text("motor.stream"),
                                     work.instance(agent_a),
                                     0,
                                     AYNI_GRANT_PERMANENT,
                                     0,
                                     0,
                                     {sizeof(ayni_scope_v1), 0, nullptr}};
    auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    auto step = [&] {
        work.node().sim().advance_us(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
        work.node().sim().complete_storage();
        work.node().step();
    };
    step();
    step();
    std::array<ayni_value_node_v1, stream_nodes> value{};
    value[0].kind = AYNI_VALUE_OBJECT;
    value[0].subtree_end = stream_nodes;
    const int64_t fields[]{-60, 60, -60, 60, 40, 40, 5000};
    for (uint32_t at = 1; at < stream_nodes; ++at) {
        value[at].kind = AYNI_VALUE_I64;
        value[at].field_id = at;
        value[at].subtree_end = at + 1;
        value[at].value.integer = fields[at - 1];
    }
    bool requested = false;
    ayni_status_t request_status = AYNI_NOT_READY;
    uint32_t decisions = 0;
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &out) {
        ++decisions;
        auto request = ayni_call_request_v1{};
        request.struct_size = sizeof(request);
        request.kind = AYNI_POLICY_REQUEST_CALL;
        request.binding_id = text("motor.stream");
        request.binding_version = motor_version;
        request.step_key = text("open");
        request.value = {value.data(), uint32_t(value.size())};
        const auto status = out.request_call(out.call_context, &request);
        request_status = status;
        requested = status == AYNI_OK;
        return status;
    };
    const auto created = work.create(
        WorkNode::create_body(work.instance(agent_a), 0, "motor.test", 1, {}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(created.status, AYNI_OK);
    for (unsigned at = 0; at < 20 && !requested; ++at) {
        step();
    }
    if (!requested) {
        const auto goal = work.goal(created.result.object.id);
        std::printf("motor fixture decisions=%u request_status=%d goal=%u task=%llu\n", decisions,
                    request_status, goal.lifecycle, (unsigned long long)goal.last_task_id);
    }
    ASSERT_TRUE(requested);
    step(); // Dispatch the opener, retaining its Provider completion before handle delivery.
    EXPECT_FALSE(motor.sessions_drained());
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    for (unsigned at = 0; at < 50 && work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED; ++at) {
        ASSERT_EQ(motor.drain_sessions(), AYNI_OK);
        step();
    }
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_TRUE(motor.sessions_drained());
}

namespace wifichat {
struct MotorSessionTestPeer {
    static ayni::runtime::OperationSession &activity(Motor &motor) { return motor.activity_; }
    static void activated(Motor &motor, uint64_t seq, MotorStreamInput input) {
        motor.active_start_seq_ = seq;
        motor.active_stream_ = input;
    }
};
} // namespace wifichat
TEST(WifiMotorSessions, StopAfterActivationBeforeDispatchClosesOldOperationAndAllowsNewOpen) {
    // ADR-0114: source Call has already delivered its handle; stop must cover the independent
    // operation before its internal ACTION can acquire a post-stop nonce/start identity.
    using namespace ayni::test;
    using namespace wifichat;
    using Activity = ayni::runtime::OperationSession;
    WorkNode work;
    ControlMailbox mailbox;
    Transport transport(mailbox);
    Motor motor(transport);
    ayni::Workspace workspace;
    ASSERT_EQ(workspace.create(ayni_value_workspace_requirements), AYNI_OK);
    auto &config = work.node().config();
    config.capability_capacity = config.call_capability_capacity = config.binding_capacity = 12;
    config.provider_capacity = 4;
    config.strategy_capacity = 4;
    config.goal_type_capacity = 6;
    config.goal_parameter_max_bytes = 1024;
    config.call_capacity = config.resource_request_capacity = 8;
    config.resource_capacity = 4;
    config.executor_capacity = config.execution_binding_capacity = 2;
    config.scene_capacity = 2;
    config.call_input_max_bytes = motor_input_bytes_max;
    config.call_result_max_bytes = motor_reply_bytes_max;
    uint64_t command = 100;
    struct Commands {
        WorkNode *work;
        uint64_t *command;
    } context{&work, &command};
    work.additional_registration = [&](ayni_kernel_t *kernel) {
        ASSERT_EQ(motor.configure(kernel), AYNI_OK);
        Activity::Commands commands;
        commands.context = &context;
        commands.submit = [](void *ctx, ayni_intent_kind_t kind, const void *body, uint64_t *id,
                             uint32_t revision) {
            auto &state = *static_cast<Commands *>(ctx);
            auto intent = state.work->intent(kind, body, revision);
            intent.command_id = ++*state.command;
            const auto submitted = state.work->submit(intent);
            if (submitted.status == AYNI_OK && id) {
                *id = submitted.result.object.id;
            }
            return Activity::SubmitResult{submitted.status, submitted.error.reason};
        };
        ASSERT_EQ(motor.configure_session(kernel, workspace.get(), commands), AYNI_OK);
    };
    work.run();
    struct ResetClock {
        ~ResetClock() { esp_test_time_us = -1; }
    } reset;
    auto &activity = MotorSessionTestPeer::activity(motor);
    const int64_t fields[]{-60, 60, -60, 60, 40, 40, 5000};
    std::array<ayni_scope_field_v1, stream_field_count> allowed{};
    for (uint32_t at = 0; at < allowed.size(); ++at) {
        auto &field = allowed[at];
        field.struct_size = sizeof(field);
        field.field_id = at + 1;
        field.allowed.struct_size = sizeof(field.allowed);
        field.allowed.kind = AYNI_AUTHORITY_I64;
        field.allowed.unit = text(at + 1 == stream_duration ? "ms" : "percent");
        field.allowed.values.integer = {fields[at], fields[at]};
    }
    const ayni_grant_create_v1 grant{
        sizeof(grant),
        motor_version,
        text(stream_execute),
        work.instance(agent_a),
        0,
        AYNI_GRANT_PERMANENT,
        0,
        0,
        {sizeof(ayni_scope_v1), uint32_t(allowed.size()), allowed.data()}};
    auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    bool motor_owner = false;
    auto step = [&] {
        work.node().sim().advance_us(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
        work.node().sim().complete_storage();
        esp_test_time_us = int64_t(work.node().sim().now_us());
        work.node().step();
        return motor_owner
                   ? motor.advance_sessions(work.kernel(), work.node().info().boot_epoch,
                                            work.node().info().durable_seq)
                   : activity.advance(work.node().info().durable_seq, work.node().sim().now_us());
    };
    for (unsigned at = 0; at < 3; ++at) {
        ASSERT_EQ(step(), AYNI_OK);
    }
    std::array<ayni_value_node_v1, stream_nodes> nodes{};
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = stream_nodes;
    for (uint32_t at = 1; at < stream_nodes; ++at) {
        nodes[at].kind = AYNI_VALUE_I64;
        nodes[at].field_id = at;
        nodes[at].subtree_end = at + 1;
        nodes[at].value.integer = fields[at - 1];
    }
    MotorStreamInput input;
    ASSERT_TRUE(read_motor_stream({nodes.data(), stream_nodes}, input));
    const auto old_start = mailbox.reserve_start();
    ASSERT_EQ(activity.open(work.node().info().boot_epoch, work.instance(agent_a),
                            {nodes.data(), stream_nodes}, 5000000),
              AYNI_OK);
    for (unsigned at = 0;
         at < 200 && activity.snapshot().phase != Activity::Phase::ready_to_activate; ++at) {
        ASSERT_EQ(step(), AYNI_OK);
    }
    ASSERT_EQ(activity.snapshot().phase, Activity::Phase::ready_to_activate)
        << "status=" << activity.snapshot().status << " reason=" << activity.snapshot().reason;
    ASSERT_EQ(activity.activate(activity.snapshot().handle, work.node().sim().now_us()), AYNI_OK);
    MotorSessionTestPeer::activated(motor, old_start, input);
    motor_owner = true;
    const auto stop = mailbox.request_stop(ControlStopKind::soft_stop);
    mailbox.confirm_stop(stop, ControlMode::armed, 0, work.node().sim().now_us());
    // No intervening Core step: the operation owner observes stop before physical dispatch.
    ASSERT_EQ(motor.advance_sessions(work.kernel(), work.node().info().boot_epoch,
                                     work.node().info().durable_seq),
              AYNI_OK);
    for (unsigned at = 0; at < 200 && !motor.sessions_drained(); ++at) {
        ASSERT_EQ(step(), AYNI_OK);
    }
    EXPECT_TRUE(motor.sessions_drained());
    EXPECT_EQ(activity.snapshot().phase, Activity::Phase::idle);
    EXPECT_TRUE(mailbox.target_blocked(old_start));
    EXPECT_FALSE(mailbox.target_blocked(mailbox.reserve_start()));
    ASSERT_EQ(activity.open(work.node().info().boot_epoch, work.instance(agent_a),
                            {nodes.data(), stream_nodes}, 5000000),
              AYNI_OK);
    activity.close_all();
    for (unsigned at = 0; at < 200 && !motor.sessions_drained(); ++at) {
        ASSERT_EQ(step(), AYNI_OK);
    }
}
