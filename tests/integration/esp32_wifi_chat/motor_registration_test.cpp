// ADR-0114: exercise production catalog and Provider handoff without a physical executor.
#include "motor.h"
#include "work_node.h"
#include <ayni/workspace.hpp>
#include <cstdio>
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
                                                   const MotorCommand &, uint64_t, uint64_t) {
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
