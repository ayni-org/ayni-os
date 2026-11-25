// ADR-0112: Phase 3 Core verification (ABI 0.59, schema 11 unchanged):
// 1. AYNI_FORM_SESSION multi-row bounding permit envelope + short-lease renewal (0 WAL),
//    while preserving ADR-0084 list rejection on AYNI_FORM_PROCEDURE.
// 2. Lock-free triple-buffered ObservationSlots concurrent publish (0 BUSY) & same-step drain.
// 3. ReflexRule SOFT_STOP_RESOURCE (same-step stop, 0 WAL, INFERRED rejected, DEGRADED/Scene
//    gated, Tier 2 DURABLE protected, stop-failure fail-closed quarantine).
// 4. ReflexRule STALE_OR_GAP fail-safe (next_deadline_us auto-wakeup + observation gap trigger).
// 5. ReflexRule PREEMPT_CALL (zero-LLM, zero-WAL, CallTable recycling, EDGE_TRIGGERED/cooldown,
//    and strict Grant/Scope/Agent/Scene preflight enforcement).
// 6. Runtime volatile toggle (ayni_set_reflex_enabled, 0 WAL) + schema 11 reboot compatibility.

#include "../support/latency_samples.h"
#include "foundation/integer_affine.h"
#include "work_node.h"
#include <ayni/audit.h>

#include <ayni/calls.h>
#include <ayni/execution.h>
#include <ayni/observations.h>
#include <ayni/reflex.h>
#include <ayni/scenes.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace ayni::test;

constexpr uint64_t step_tick_us = 10000;
constexpr uint64_t control_domain = 88;
constexpr uint32_t session_lease_us = 200000;
constexpr uint32_t session_poll_us = 50000;
constexpr uint64_t obstacle_validity_us = 500000;

const ayni_control_range_v1 *find_envelope_field(const ayni_control_envelope_v1 &env,
                                                 const char *name) {
    const std::string_view wanted(name);
    for (uint32_t at = 0; at < env.count; ++at) {
        const auto &field = env.fields[at];
        if (std::string_view(field.field_id.data, field.field_id.size) == wanted) {
            return &field;
        }
    }
    return nullptr;
}

struct MotorSegmentInput {
    int64_t m1 = 0;
    int64_t m2 = 0;
    int64_t duration_ms = 0;
};

// Encodes canonical v3 motor.run value tree: {"segments": [{"m1", "m2", "duration_ms"}, ...]}.
std::vector<ayni_value_node_v1> make_segments_value(const std::vector<MotorSegmentInput> &segs) {
    const uint32_t total = static_cast<uint32_t>(2U + segs.size() * 4U);
    std::vector<ayni_value_node_v1> nodes(total);
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = total;
    nodes[1].kind = AYNI_VALUE_LIST;
    nodes[1].field_id = 1;
    nodes[1].subtree_end = total;
    for (std::size_t at = 0; at < segs.size(); ++at) {
        const uint32_t base = static_cast<uint32_t>(2U + at * 4U);
        nodes[base].kind = AYNI_VALUE_OBJECT;
        nodes[base].subtree_end = base + 4U;
        nodes[base + 1].kind = AYNI_VALUE_I64;
        nodes[base + 1].field_id = 1;
        nodes[base + 1].subtree_end = base + 2U;
        nodes[base + 1].value.integer = segs[at].m1;
        nodes[base + 2].kind = AYNI_VALUE_I64;
        nodes[base + 2].field_id = 2;
        nodes[base + 2].subtree_end = base + 3U;
        nodes[base + 2].value.integer = segs[at].m2;
        nodes[base + 3].kind = AYNI_VALUE_I64;
        nodes[base + 3].field_id = 3;
        nodes[base + 3].subtree_end = base + 4U;
        nodes[base + 3].value.integer = segs[at].duration_ms;
    }
    return nodes;
}

std::array<ayni_schema_node_v1, 6> motor_v3_schema() {
    std::array<ayni_schema_node_v1, 6> nodes{};
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = 6;
    nodes[1].kind = AYNI_VALUE_LIST;
    nodes[1].field_id = 1;
    nodes[1].subtree_end = 6;
    nodes[1].flags = AYNI_SCHEMA_REQUIRED;
    nodes[1].name = text("segments");
    nodes[1].min_count = 1;
    nodes[1].max_count = 8;
    nodes[2].kind = AYNI_VALUE_OBJECT;
    nodes[2].subtree_end = 6;
    const char *names[3] = {"motor1_percent", "motor2_percent", "duration_ms"};
    const char *units[3] = {"percent", "percent", "ms"};
    const int64_t mins[3] = {-90, -90, 50};
    const int64_t maxs[3] = {90, 90, 60000};
    for (uint32_t at = 0; at < 3; ++at) {
        auto &n = nodes[at + 3];
        n.kind = AYNI_VALUE_I64;
        n.field_id = at + 1;
        n.subtree_end = at + 4;
        n.flags = AYNI_SCHEMA_REQUIRED;
        n.name = text(names[at]);
        n.unit = text(units[at]);
        n.min_i64 = mins[at];
        n.max_i64 = maxs[at];
    }
    return nodes;
}

// Colocated SESSION executor + provider stub that records every issued permit envelope and
// renewal, supports instant inline stop proofs, and can simulate hardware stop failure.
struct SessionReflexExecutor {
    bool fail_stop = false;
    bool auto_cancel_with_proof = true;
    uint32_t challenge_calls = 0;
    uint32_t renew_calls = 0;
    uint32_t stop_calls = 0;
    uint32_t status_calls = 0;
    uint32_t submit_calls = 0;
    uint32_t cancel_calls = 0;
    uint64_t next_nonce = 100;

    struct Active {
        ayni_attempt_ref_v1 attempt{};
        uint64_t call_id = 0;
        ayni_execution_permit_v1 permit{};
        ayni_provider_completion_t *completion = nullptr;
        bool stopped = false;
        bool posted = false;
    };
    std::optional<Active> active;
    ayni_execution_permit_v1 last_permit{};
    uint64_t last_permit_id = 0;
    uint64_t last_operation_id = 0;
    bool last_stopped = false;
    std::vector<uint64_t> submitted_calls;

    static ayni_status_t on_challenge(void *ctx, ayni_attempt_ref_v1 request,
                                      const ayni_permit_epochs_v1 *,
                                      ayni_executor_completion_t *completion) {
        auto &self = *static_cast<SessionReflexExecutor *>(ctx);
        ++self.challenge_calls;
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_CHALLENGE;
        resp.request = request;
        resp.status = AYNI_OK;
        resp.output.nonce = {control_domain, self.next_nonce++};
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_renew(void *ctx, ayni_attempt_ref_v1 request,
                                  const ayni_permit_renewal_v1 *,
                                  ayni_executor_completion_t *completion) {
        auto &self = *static_cast<SessionReflexExecutor *>(ctx);
        ++self.renew_calls;
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_RENEW;
        resp.request = request;
        resp.status = AYNI_OK;
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_stop(void *ctx, ayni_attempt_ref_v1 request, uint64_t permit_id,
                                 ayni_executor_completion_t *completion) {
        auto &self = *static_cast<SessionReflexExecutor *>(ctx);
        ++self.stop_calls;
        if (!self.fail_stop) {
            if (self.active && self.active->permit.permit_id == permit_id) {
                self.active->stopped = true;
            }
            if (self.last_permit_id == permit_id) {
                self.last_stopped = true;
            }
        }
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_STOP;
        resp.request = request;
        resp.status = self.fail_stop ? AYNI_INTERNAL : AYNI_OK;
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_status(void *ctx, ayni_attempt_ref_v1 request, uint64_t permit_id,
                                   ayni_executor_completion_t *completion) {
        auto &self = *static_cast<SessionReflexExecutor *>(ctx);
        ++self.status_calls;
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_STATUS;
        resp.request = request;
        const bool match_active = self.active && self.active->permit.permit_id == permit_id;
        const bool match_last = self.last_permit_id == permit_id;
        if (!match_active && !match_last) {
            resp.status = AYNI_NOT_FOUND;
            return ayni_executor_post(completion, &resp);
        }
        const bool is_stopped =
            !self.fail_stop && (match_active ? self.active->stopped : self.last_stopped);
        const uint64_t op_id =
            match_active ? self.active->permit.operation_id : self.last_operation_id;
        resp.status = AYNI_OK;
        resp.output.status.struct_size = sizeof(ayni_executor_status_v1);
        resp.output.status.target = AYNI_TARGET_UNVERIFIED;
        resp.output.status.control.struct_size = sizeof(resp.output.status.control);
        resp.output.status.control.phase = is_stopped ? AYNI_CONTROL_STOPPED : AYNI_CONTROL_ACTIVE;
        resp.output.status.control.stop_cause =
            is_stopped ? AYNI_CONTROL_STOP_REQUESTED : AYNI_CONTROL_STOP_NONE;
        resp.output.status.control.permit_id = permit_id;
        resp.output.status.control.operation_id = op_id;
        return ayni_executor_post(completion, &resp);
    }

    static ayni_submit_result_t on_submit(void *ctx, const ayni_provider_request_v1 *request,
                                          ayni_provider_completion_t *completion) {
        auto &self = *static_cast<SessionReflexExecutor *>(ctx);
        ++self.submit_calls;
        if (request == nullptr || request->execution_permit == nullptr || completion == nullptr) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.active = Active{request->attempt,
                             request->logical_call_id,
                             *request->execution_permit,
                             completion,
                             false,
                             false};
        self.last_permit = *request->execution_permit;
        self.last_permit_id = request->execution_permit->permit_id;
        self.last_operation_id = request->execution_permit->operation_id;
        self.last_stopped = false;
        self.submitted_calls.push_back(request->logical_call_id);
        return AYNI_SUBMIT_ACCEPTED;
    }

    static ayni_submit_result_t on_cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<SessionReflexExecutor *>(ctx);
        ++self.cancel_calls;
        if (!self.active || self.active->posted ||
            self.active->attempt.request_id != attempt.request_id) {
            return AYNI_SUBMIT_ACCEPTED;
        }
        if (self.fail_stop) {
            self.finish_active(AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_CALL_EFFECT_UNKNOWN,
                               nullptr, false);
        } else if (self.auto_cancel_with_proof) {
            self.finish_active(AYNI_CALL_CANCELLED, AYNI_OK, AYNI_CALL_EFFECT_PARTIAL, "stopped",
                               true);
        }
        return AYNI_SUBMIT_ACCEPTED;
    }

    void finish_active(ayni_call_outcome_t outcome, ayni_status_t status, ayni_call_effect_t effect,
                       const char *reply, bool with_stopped_proof) {
        if (!active || active->posted) {
            return;
        }
        active->posted = true;
        active->stopped = with_stopped_proof;
        last_stopped = with_stopped_proof;
        ayni_provider_result_v1 result{};
        result.struct_size = sizeof(result);
        result.outcome = outcome;
        result.attempt = active->attempt;
        result.status = status;
        result.effect = effect;
        if (reply != nullptr && outcome != AYNI_CALL_ABANDONED) {
            result.result = text(reply);
        }
        ayni_executor_status_v1 inline_status{};
        if (with_stopped_proof && outcome != AYNI_CALL_ABANDONED) {
            inline_status.struct_size = sizeof(inline_status);
            inline_status.target = AYNI_TARGET_UNVERIFIED;
            inline_status.control.struct_size = sizeof(inline_status.control);
            inline_status.control.permit_id = active->permit.permit_id;
            inline_status.control.operation_id = active->permit.operation_id;
            inline_status.control.phase = AYNI_CONTROL_STOPPED;
            inline_status.control.stop_cause = AYNI_CONTROL_STOP_REQUESTED;
            result.execution_status = &inline_status;
        }
        EXPECT_EQ(ayni_provider_post(active->completion, &result), AYNI_OK);
    }

    ayni_executor_v1 ports() {
        return {sizeof(ayni_executor_v1),
                1,
                text("motor.exec"),
                control_domain,
                1,
                1,
                1,
                1,
                session_lease_us,
                session_lease_us,
                session_lease_us,
                session_poll_us,
                this,
                on_challenge,
                on_renew,
                on_stop,
                on_status,
                AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF,
                0};
    }
};

struct ReflexFixture {
    WorkNode work;
    SessionReflexExecutor executor;
    uint64_t command_id = 100;
    uint64_t now_us = 1000000;
    bool register_scene_flag = true;
    std::function<void(ayni_kernel_t *)> extra_configure;

    void configure_defaults() {
        auto &cfg = work.node().config();
        cfg.goal_type_capacity = 6;
        cfg.capability_capacity = 8;
        cfg.grant_capacity = 12;
        cfg.goal_capacity = 12;
        cfg.task_capacity = 12;
        cfg.command_capacity = 24;
        cfg.resource_capacity = 4;
        cfg.resource_request_capacity = 8;
        cfg.call_capability_capacity = 6;
        cfg.provider_capacity = 4;
        cfg.binding_capacity = 6;
        cfg.call_capacity = 8;
        cfg.call_input_max_bytes = 768;
        cfg.call_result_max_bytes = 256;
        cfg.business_contract_max_bytes = 4096;
        cfg.device_capacity = 2;
        cfg.component_capacity = 2;
        cfg.executor_capacity = 2;
        cfg.execution_binding_capacity = 4;
        cfg.scene_capacity = 4;
        cfg.observation_source_capacity = 4;
        cfg.observation_ingress_capacity = 16;
        cfg.reflex_capacity = 8;
        cfg.reflex_event_capacity = 16;
    }

    void register_motor_stack(ayni_kernel_t *kernel) {
        // Device + Component + Resource (motor.actuator).
        const ayni_device_desc_v1 dev{sizeof(dev), 0, text("bot.dev")};
        ASSERT_EQ(ayni_register_device(kernel, &dev), AYNI_OK);
        const ayni_component_desc_v1 comp{sizeof(comp), 0, text("bot.motor"), text("bot.dev")};
        ASSERT_EQ(ayni_register_component(kernel, &comp), AYNI_OK);
        const ayni_string_v1 comps[1] = {text("bot.motor")};
        const ayni_resource_desc_v1 res{sizeof(res),
                                        AYNI_RESOURCE_EXCLUSIVE,
                                        text("motor.actuator"),
                                        1,
                                        AYNI_RESOURCE_ACTUATOR,
                                        1,
                                        0,
                                        comps,
                                        {}};
        ASSERT_EQ(ayni_register_resource(kernel, &res), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, text("motor.actuator"), AYNI_RESOURCE_ONLINE),
                  AYNI_OK);

        // Capability motor.run v3 (fields 1=m1, 2=m2, 3=duration_ms).
        std::array<ayni_authority_field_desc_v1, 3> fields{};
        const char *units[3] = {"percent", "percent", "ms"};
        const int64_t mins[3] = {-90, -90, 50};
        const int64_t maxs[3] = {90, 90, 60000};
        for (uint32_t at = 0; at < 3; ++at) {
            fields[at].struct_size = sizeof(fields[at]);
            fields[at].field_id = at + 1;
            fields[at].usage = AYNI_AUTHORITY_PARAMETER;
            fields[at].domain.struct_size = sizeof(fields[at].domain);
            fields[at].domain.kind = AYNI_AUTHORITY_I64;
            fields[at].domain.unit = text(units[at]);
            fields[at].domain.values.integer = {mins[at], maxs[at]};
        }
        const ayni_capability_desc_v1 cap{sizeof(cap), 3, text("motor.run"), 3, 0, fields.data()};
        ASSERT_EQ(ayni_register_capability(kernel, &cap), AYNI_OK);

        // v3 segments[] BusinessContract with LIST_ITEM projections.
        const auto schema_nodes = motor_v3_schema();
        ayni_schema_node_v1 result_schema{};
        result_schema.kind = AYNI_VALUE_TEXT;
        result_schema.subtree_end = 1;
        result_schema.max_count = 128;
        std::array<ayni_authority_projection_v1, 3> projections{};
        for (uint32_t at = 0; at < 3; ++at) {
            projections[at].field_id = at + 1;
            projections[at].path_count = 3;
            projections[at].path[0] = 1;
            projections[at].path[1] = AYNI_PROJECTION_LIST_ITEM;
            projections[at].path[2] = at + 1;
            projections[at].unit = text(units[at]);
        }
        const ayni_business_contract_v1 business{
            {schema_nodes.data(), static_cast<uint32_t>(schema_nodes.size())},
            {&result_schema, 1},
            3,
            0,
            projections.data()};

        const ayni_call_resource_v1 req{text("motor.actuator"), 1, 0};
        const ayni_call_capability_v1 call_cap{
            sizeof(call_cap),
            3,
            text("motor.run"),
            {sizeof(ayni_call_contract_v1), AYNI_CALL_ACTION, AYNI_CALL_TOOL,
             AYNI_CALL_EFFECT_PHYSICAL, AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE,
             AYNI_CALL_CANCEL_REQUEST, 0, 65000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 768,
             128, 1, &req, nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
            &business};
        ASSERT_EQ(ayni_register_call_capability(kernel, &call_cap), AYNI_OK);

        // Provider + Binding.
        const ayni_provider_v1 provider{sizeof(provider),
                                        1,
                                        text("motor.provider"),
                                        text("motor.endpoint"),
                                        {},
                                        &executor,
                                        1,
                                        0,
                                        &call_cap,
                                        SessionReflexExecutor::on_submit,
                                        SessionReflexExecutor::on_cancel,
                                        nullptr,
                                        nullptr};
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);

        const ayni_binding_v1 binding{sizeof(binding),
                                      3,
                                      text("motor.run"),
                                      text("motor.run"),
                                      3,
                                      1,
                                      text("motor.provider"),
                                      text("motor.endpoint"),
                                      {},
                                      0,
                                      0,
                                      nullptr};
        ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);

        // Trusted observation source sensor.obstacle_mm (MEASURED).
        const ayni_observation_source_v1 obstacle{
            sizeof(obstacle),          1,          text("sensor.obstacle_mm"), AYNI_OBSERVATION_I64,
            AYNI_OBSERVATION_MEASURED, text("mm"), obstacle_validity_us};
        ASSERT_EQ(ayni_register_observation_source(kernel, &obstacle), AYNI_OK);

        // Scene motor.manual-bench with tighter bounds [-50, 50] and duration [50, 2000].
        if (register_scene_flag) {
            std::array<ayni_scope_field_v1, 3> scene_fields{};
            const int64_t smins[3] = {-50, -50, 50};
            const int64_t smaxs[3] = {50, 50, 2000};
            for (uint32_t at = 0; at < 3; ++at) {
                scene_fields[at].struct_size = sizeof(scene_fields[at]);
                scene_fields[at].field_id = at + 1;
                scene_fields[at].allowed = fields[at].domain;
                scene_fields[at].allowed.values.integer = {smins[at], smaxs[at]};
            }
            const ayni_scene_rule_v1 rule{
                text("motor.run"), 3, 0, {sizeof(ayni_scope_v1), 3, scene_fields.data()}};
            const ayni_scene_desc_v1 scene{sizeof(scene), 1,      text("motor.manual-bench"), 1, 0,
                                           &rule,         nullptr};
            ASSERT_EQ(ayni_register_scene(kernel, &scene), AYNI_OK);
        }

        // Executor + SESSION ExecutionBinding (with 3 list-projected fields).
        const auto exec_ports = executor.ports();
        ASSERT_EQ(ayni_register_executor(kernel, &exec_ports), AYNI_OK);
        const std::array<ayni_execution_field_v1, 3> exec_fields{
            ayni_execution_field_v1{text("motor1_percent"), text("percent"), 1, 1},
            ayni_execution_field_v1{text("motor2_percent"), text("percent"), 2, 2},
            ayni_execution_field_v1{text("duration_ms"), text("ms"), 3, 3},
        };
        const ayni_execution_binding_v1 exec_binding{sizeof(exec_binding),
                                                     3,
                                                     text("motor.run"),
                                                     text("motor.exec"),
                                                     1,
                                                     1,
                                                     text("motor.manual-bench"),
                                                     AYNI_FORM_SESSION,
                                                     session_lease_us,
                                                     100000,
                                                     3,
                                                     exec_fields.data()};
        ASSERT_EQ(ayni_register_execution_binding(kernel, &exec_binding), AYNI_OK);

        // Foreground GoalType exporting motor.run v3 requirements so Tasks can invoke motor.run.
        auto goal_type = goal_type_desc("motor.goal", 1);
        goal_type.validate = [](void *, ayni_bytes_v1) { return AYNI_OK; };
        goal_type.export_requirements = [](void *, ayni_bytes_v1,
                                           const ayni_goal_requirement_sink_v1 *sink) {
            std::array<ayni_scope_field_v1, 3> req_fields{};
            const char *req_units[3] = {"percent", "percent", "ms"};
            const int64_t req_mins[3] = {-60, -60, 50};
            const int64_t req_maxs[3] = {60, 60, 5000};
            for (uint32_t at = 0; at < 3; ++at) {
                req_fields[at].struct_size = sizeof(req_fields[at]);
                req_fields[at].field_id = at + 1;
                req_fields[at].allowed.struct_size = sizeof(req_fields[at].allowed);
                req_fields[at].allowed.kind = AYNI_AUTHORITY_I64;
                req_fields[at].allowed.unit = text(req_units[at]);
                req_fields[at].allowed.values.integer = {req_mins[at], req_maxs[at]};
            }
            const ayni_goal_requirement_v1 need{
                sizeof(need), 3, text("motor.run"), {sizeof(ayni_scope_v1), 3, req_fields.data()}};
            return sink->emit(sink->ctx, &need);
        };
        goal_type.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT, 0};
        ASSERT_EQ(ayni_register_goal_type(kernel, &goal_type), AYNI_OK);
    }

    void start() {
        configure_defaults();
        work.additional_registration = [this](ayni_kernel_t *kernel) {
            register_motor_stack(kernel);
            if (extra_configure) {
                extra_configure(kernel);
            }
        };
        work.run();
        if (work.node().sim().now_us() < 1000000U) {
            work.node().sim().advance_us(1000000U - work.node().sim().now_us());
        }
        now_us = work.node().sim().now_us();
    }

    std::size_t log_batches() { return work.node().sim().log().size(); }

    ayni_step_report_v1 step(uint64_t advance_us = step_tick_us) {
        if (advance_us > 0U) {
            now_us += advance_us;
            work.node().sim().advance_us(advance_us);
        }
        work.node().sim().complete_storage();
        const auto report = work.node().step();
        EXPECT_NE(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        return report;
    }

    void flush_durable() {
        step(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
        step(step_tick_us);
    }

    void set_scene(bool enable) {
        ayni_scene_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_scene(work.kernel(), text("motor.manual-bench"), &info), AYNI_OK);
        const ayni_scene_set_enabled_v1 body{sizeof(body), enable ? 1U : 0U,
                                             text("motor.manual-bench"), enable ? 1U : 0U, 0};
        auto intent = work.intent(AYNI_INTENT_SCENE_SET_ENABLED, &body, info.revision);
        intent.command_id = ++command_id;
        ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        flush_durable();
    }

    uint64_t grant_motor(uint64_t agent, int64_t lo = -60, int64_t hi = 60,
                         int64_t dur_max = 5000) {
        std::array<ayni_scope_field_v1, 3> fields{};
        const char *units[3] = {"percent", "percent", "ms"};
        const int64_t mins[3] = {lo, lo, 50};
        const int64_t maxs[3] = {hi, hi, dur_max};
        for (uint32_t at = 0; at < 3; ++at) {
            fields[at].struct_size = sizeof(fields[at]);
            fields[at].field_id = at + 1;
            fields[at].allowed.struct_size = sizeof(fields[at].allowed);
            fields[at].allowed.kind = AYNI_AUTHORITY_I64;
            fields[at].allowed.unit = text(units[at]);
            fields[at].allowed.values.integer = {mins[at], maxs[at]};
        }
        ayni_grant_create_v1 create{};
        create.struct_size = sizeof(create);
        create.capability_version = 3;
        create.capability_id = text("motor.run");
        create.subject_instance_id = work.instance(agent);
        create.lifetime = AYNI_GRANT_PERMANENT;
        create.scope = {sizeof(ayni_scope_v1), 3, fields.data()};
        auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &create);
        intent.command_id = ++command_id;
        const auto submitted = work.submit(intent);
        EXPECT_EQ(submitted.status, AYNI_OK);
        flush_durable();
        return submitted.result.object.id;
    }

    void revoke_grant(uint64_t grant_id) {
        ayni_grant_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_grant(work.kernel(), grant_id, &info), AYNI_OK);
        const ayni_grant_revoke_v1 body{sizeof(body), 0, grant_id};
        auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &body, info.revision);
        intent.command_id = ++command_id;
        ASSERT_EQ(work.submit(intent).status, AYNI_OK);
        flush_durable();
    }

    ayni_status_t
    publish_obstacle_latest(uint64_t seq, int64_t mm,
                            ayni_observation_quality_t quality = AYNI_OBSERVATION_GOOD,
                            uint64_t captured_override = 0) {
        ayni_observation_post_v1 post{};
        post.struct_size = sizeof(post);
        post.boot_epoch = work.node().info().boot_epoch;
        post.sample.struct_size = sizeof(post.sample);
        post.sample.source_version = 1;
        post.sample.source_id = text("sensor.obstacle_mm");
        post.sample.generation = 1;
        post.sample.sequence = seq;
        post.sample.captured_at_us = captured_override != 0U ? captured_override : now_us;
        post.sample.valid_for_us = obstacle_validity_us;
        post.sample.quality = quality;
        post.sample.value = mm;
        return ayni_publish_observation_latest(work.kernel(), &post);
    }

    uint64_t start_motor_goal(const std::vector<MotorSegmentInput> &segs, const char *step_key,
                              ayni_call_preemption_t preemption = AYNI_CALL_PREEMPT_NONE) {
        auto encoded = make_segments_value(segs);
        work.strategy().answer = [encoded, step_key, preemption](const ayni_task_snapshot_v1 &snap,
                                                                 ayni_policy_output_v1 &out) {
            ayni_task_call_result_v1 prior{};
            prior.struct_size = sizeof(prior);
            if (snap.call_result(snap.call_context, text(step_key), &prior) == AYNI_OK) {
                return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
            }
            ayni_call_request_v1 req{};
            req.struct_size = sizeof(req);
            req.kind = AYNI_POLICY_REQUEST_CALL;
            req.binding_version = 3;
            req.step_key = text(step_key);
            req.binding_id = text("motor.run");
            req.value = {encoded.data(), static_cast<uint32_t>(encoded.size())};
            req.mode = AYNI_CALL_MODE_FOREGROUND;
            req.preemption = preemption;
            return out.request_call(out.call_context, &req);
        };
        const auto body = WorkNode::create_body(work.instance(agent_a), 0, "motor.goal", 1, {},
                                                AYNI_TRIGGER_ONCE);
        const auto accepted = work.create(body);
        EXPECT_EQ(accepted.status, AYNI_OK);
        return accepted.result.object.id;
    }

    ayni_resource_info_v1 resource_info() {
        ayni_resource_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_resource(work.kernel(), text("motor.actuator"), &info), AYNI_OK);
        return info;
    }

    ayni_reflex_info_v1 reflex_info(const char *rule_id) {
        ayni_reflex_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_reflex_info(work.kernel(), text(rule_id), &info), AYNI_OK);
        return info;
    }
};

// Test 1 (ADR-0112): AYNI_FORM_PROCEDURE still rejects list-projected execution fields (ADR-0084),
// while AYNI_FORM_SESSION computes the exact multi-row bounding permit envelope, checks every row
// against Grant & Scene, and renews short leases transparently with 0 WAL appends.
TEST(ReflexAndSession, SessionMultiRowBoundingEnvelopeAndRenewal) {
    // First confirm AYNI_FORM_PROCEDURE still rejects AYNI_PROJECTION_LIST_ITEM at ayni_start().
    {
        ReflexFixture probe;
        probe.configure_defaults();
        probe.work.additional_registration = [&](ayni_kernel_t *kernel) {
            probe.register_motor_stack(kernel);
            const ayni_binding_v1 proc_bind{sizeof(proc_bind),
                                            1,
                                            text("motor.proc"),
                                            text("motor.run"),
                                            3,
                                            1,
                                            text("motor.provider"),
                                            text("motor.endpoint"),
                                            {},
                                            0,
                                            0,
                                            nullptr};
            ASSERT_EQ(ayni_register_binding(kernel, &proc_bind), AYNI_OK);
            const std::array<ayni_execution_field_v1, 3> exec_fields{
                ayni_execution_field_v1{text("motor1_percent"), text("percent"), 1, 1},
                ayni_execution_field_v1{text("motor2_percent"), text("percent"), 2, 2},
                ayni_execution_field_v1{text("duration_ms"), text("ms"), 3, 3},
            };
            const ayni_execution_binding_v1 bad_proc{sizeof(bad_proc),
                                                     1,
                                                     text("motor.proc"),
                                                     text("motor.exec"),
                                                     1,
                                                     1,
                                                     text("motor.manual-bench"),
                                                     AYNI_FORM_PROCEDURE,
                                                     session_lease_us,
                                                     100000,
                                                     3,
                                                     exec_fields.data()};
            ASSERT_EQ(ayni_register_execution_binding(kernel, &bad_proc), AYNI_OK);
        };
        probe.work.node().create();
        EXPECT_EQ(ayni_start(probe.work.kernel()), AYNI_UNSUPPORTED);
    }

    ReflexFixture fix;
    fix.start();
    fix.grant_motor(agent_a, -60, 60, 5000);
    fix.set_scene(true);

    // Row 2 has m1=55, which is inside Grant [-60, 60] but outside Scene [-50, 50] -> denied at
    // admission before any executor submit, pausing the Task with AYNI_REASON_EXECUTION_SCENE.
    {
        auto segs = make_segments_value({{20, 20, 200}, {55, 10, 200}});
        ayni_status_t request_status = AYNI_OK;
        fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &,
                                         ayni_policy_output_v1 &out) {
            ayni_call_request_v1 req{};
            req.struct_size = sizeof(req);
            req.kind = AYNI_POLICY_REQUEST_CALL;
            req.binding_version = 3;
            req.step_key = text("bad_row");
            req.binding_id = text("motor.run");
            req.value = {segs.data(), static_cast<uint32_t>(segs.size())};
            req.mode = AYNI_CALL_MODE_FOREGROUND;
            request_status = out.request_call(out.call_context, &req);
            return request_status;
        };
        const auto body = WorkNode::create_body(fix.work.instance(agent_a), 0, "motor.goal", 1, {},
                                                AYNI_TRIGGER_ONCE);
        const auto created = fix.work.create(body);
        ASSERT_EQ(created.status, AYNI_OK);
        fix.flush_durable();
        for (int i = 0; i < 8; ++i) {
            fix.step();
        }
        EXPECT_EQ(request_status, AYNI_OK);
        const auto goal_info = fix.work.goal(created.result.object.id);
        ASSERT_NE(goal_info.last_task_id, 0U);
        const auto task_info = fix.work.task(goal_info.last_task_id);
        EXPECT_EQ(task_info.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(task_info.hold_status, AYNI_DENIED);
        EXPECT_EQ(task_info.hold_reason, AYNI_REASON_EXECUTION_SCENE);
        EXPECT_EQ(fix.executor.submit_calls, 0U);
    }

    // Valid 3-segment trajectory: bounding box should be m1:[-20,40], m2:[0,40], dur:[150,400].
    fix.start_motor_goal({{40, 40, 300}, {0, 0, 150}, {-20, 35, 400}}, "traj3");
    for (int i = 0; i < 15 && fix.executor.submit_calls == 0; ++i) {
        fix.step();
    }
    ASSERT_EQ(fix.executor.submit_calls, 1U);
    EXPECT_EQ(fix.executor.last_permit.form, AYNI_FORM_SESSION);
    ASSERT_EQ(fix.executor.last_permit.envelope.count, 3U);
    const auto *m1_range = find_envelope_field(fix.executor.last_permit.envelope, "motor1_percent");
    const auto *m2_range = find_envelope_field(fix.executor.last_permit.envelope, "motor2_percent");
    const auto *dur_range = find_envelope_field(fix.executor.last_permit.envelope, "duration_ms");
    ASSERT_NE(m1_range, nullptr);
    ASSERT_NE(m2_range, nullptr);
    ASSERT_NE(dur_range, nullptr);
    EXPECT_EQ(m1_range->minimum, -20);
    EXPECT_EQ(m1_range->maximum, 40);
    EXPECT_EQ(m2_range->minimum, 0);
    EXPECT_EQ(m2_range->maximum, 40);
    EXPECT_EQ(dur_range->minimum, 150);
    EXPECT_EQ(dur_range->maximum, 400);

    // Advance well past initial 200 ms lease -> verify transparent renewals and 0 WAL writes!
    const std::size_t appends_before = fix.log_batches();
    for (int i = 0; i < 45; ++i) {
        fix.step(10000);
    }
    EXPECT_GE(fix.executor.renew_calls, 2U);
    EXPECT_EQ(fix.log_batches(), appends_before);

    // Complete cleanly -> resource immediately FREE with 0 extra WAL writes for the EPHEMERAL call.
    fix.executor.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "done",
                               true);
    fix.step();
    EXPECT_EQ(fix.resource_info().state, AYNI_RESOURCE_FREE);
}

// Test 2 (ADR-0112): Lock-free triple-buffered ObservationSlots sustain concurrent high-rate
// publishing with 0 BUSY, preserve the latest sample on drain, and reflect it within a single step.
TEST(ReflexAndSession, ObservationSlotLockFreeConcurrentPublishAndSameStepDrain) {
    ReflexFixture fix;
    fix.start();
    const uint64_t fixed_capture = fix.now_us;
    constexpr uint64_t total_publishes = 400;
    std::atomic<uint32_t> errors{0};

    std::thread producer([&]() {
        for (uint64_t seq = 1; seq <= total_publishes; ++seq) {
            const auto st = fix.publish_obstacle_latest(seq, static_cast<int64_t>(1000 + seq),
                                                        AYNI_OBSERVATION_GOOD, fixed_capture);
            if (st != AYNI_OK) {
                errors.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    });

    // Owner steps concurrently while producer spams the slot.
    for (int i = 0; i < 20; ++i) {
        fix.step(0);
    }
    producer.join();
    // Single final drain captures the newest sample.
    fix.step(0);

    EXPECT_EQ(errors.load(), 0U);
    ayni_observation_slot_info_v1 slot_info{};
    slot_info.struct_size = sizeof(slot_info);
    ASSERT_EQ(ayni_get_observation_slots(fix.work.kernel(), &slot_info), AYNI_OK);
    EXPECT_EQ(slot_info.source_count, 1U);
    EXPECT_EQ(slot_info.published, total_publishes);
    EXPECT_GT(slot_info.applied, 0U);
    EXPECT_EQ(slot_info.discarded, 0U);
    EXPECT_EQ(slot_info.overwritten + slot_info.applied + slot_info.discarded, total_publishes);

    ayni_observation_info_v1 obs{};
    obs.struct_size = sizeof(obs);
    ASSERT_EQ(ayni_get_observation(fix.work.kernel(), text("sensor.obstacle_mm"), &obs), AYNI_OK);
    EXPECT_EQ(obs.usable, 1U);
    EXPECT_EQ(obs.reference.sequence, total_publishes);
    EXPECT_EQ(obs.value, static_cast<int64_t>(1000 + total_publishes));
}

// ADR-0112: a delayed publication cannot displace the newest unread sample.
TEST(ReflexAndSession, observation_slot_rejects_out_of_order_unread_sample) {
    ReflexFixture fix;
    fix.start();
    ASSERT_EQ(fix.publish_obstacle_latest(20, 90), AYNI_OK);
    EXPECT_EQ(fix.publish_obstacle_latest(19, 500), AYNI_STALE);
    EXPECT_EQ(fix.publish_obstacle_latest(20, 600), AYNI_STALE);

    const auto report = fix.work.node().step(1);
    EXPECT_EQ(report.items_done, 1U);
    ayni_observation_info_v1 observation{};
    observation.struct_size = sizeof(observation);
    ASSERT_EQ(ayni_get_observation(fix.work.kernel(), text("sensor.obstacle_mm"), &observation),
              AYNI_OK);
    EXPECT_EQ(observation.reference.sequence, 20U);
    EXPECT_EQ(observation.value, 90);
}

// Test 3 (ADR-0112): SOFT_STOP_RESOURCE rejects INFERRED sources at start, stops an active
// EPHEMERAL physical call in the exact same step with 0 WAL, ignores DEGRADED/disabled Scene,
// protects Tier 2 DURABLE calls, and quarantines on failed hardware stop.
TEST(ReflexAndSession, ReflexSoftStopResourceAndSafetyBoundaries) {
    // Verify INFERRED source is rejected at ayni_start().
    {
        ReflexFixture bad;
        bad.configure_defaults();
        bad.work.additional_registration = [&](ayni_kernel_t *kernel) {
            bad.register_motor_stack(kernel);
            const ayni_observation_source_v1 inferred{sizeof(inferred),
                                                      1,
                                                      text("sensor.inferred"),
                                                      AYNI_OBSERVATION_I64,
                                                      AYNI_OBSERVATION_INFERRED,
                                                      text("mm"),
                                                      500000};
            ASSERT_EQ(ayni_register_observation_source(kernel, &inferred), AYNI_OK);
            const ayni_observation_condition_v1 cond{text("sensor.inferred"), 1, 0, 0, 200, 500000};
            ayni_reflex_rule_v1 rule{};
            rule.struct_size = sizeof(rule);
            rule.version = 1;
            rule.rule_id = text("reflex.bad_inferred");
            rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
            rule.trigger_mode = AYNI_REFLEX_TRIGGER_IN_RANGE;
            rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT;
            rule.condition_count = 1;
            rule.conditions = &cond;
            rule.resource_id = text("motor.actuator");
            ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);
        };
        bad.work.node().create();
        EXPECT_EQ(ayni_start(bad.work.kernel()), AYNI_INVALID_ARGUMENT);
    }

    ReflexFixture fix;
    fix.extra_configure = [](ayni_kernel_t *kernel) {
        const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 200,
                                                 obstacle_validity_us};
        ayni_reflex_rule_v1 rule{};
        rule.struct_size = sizeof(rule);
        rule.version = 1;
        rule.rule_id = text("reflex.obstacle_stop");
        rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
        rule.trigger_mode = AYNI_REFLEX_TRIGGER_IN_RANGE;
        rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
        rule.priority = 100;
        rule.scene_id = text("motor.manual-bench");
        rule.scene_version = 1;
        rule.condition_count = 1;
        rule.conditions = &cond;
        rule.resource_id = text("motor.actuator");
        ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);
    };
    fix.start();
    fix.grant_motor(agent_a);
    fix.set_scene(true);

    // Subcase A: active EPHEMERAL physical call stopped in the SAME ayni_step(), 0 WAL.
    ayni_task_call_result_v1 call_res{};
    call_res.struct_size = sizeof(call_res);
    bool call_finished = false;
    auto segs = make_segments_value({{40, 40, 1500}});
    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        if (snap.call_result(snap.call_context, text("run1"), &call_res) == AYNI_OK) {
            call_finished = true;
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        }
        ayni_call_request_v1 req{};
        req.struct_size = sizeof(req);
        req.kind = AYNI_POLICY_REQUEST_CALL;
        req.binding_version = 3;
        req.step_key = text("run1");
        req.binding_id = text("motor.run");
        req.value = {segs.data(), static_cast<uint32_t>(segs.size())};
        req.mode = AYNI_CALL_MODE_FOREGROUND;
        return out.request_call(out.call_context, &req);
    };
    ASSERT_EQ(fix.work
                  .create(WorkNode::create_body(fix.work.instance(agent_a), 0, "motor.goal", 1, {},
                                                AYNI_TRIGGER_ONCE))
                  .status,
              AYNI_OK);
    for (int i = 0; i < 8 && fix.executor.submit_calls == 0; ++i) {
        fix.step();
    }
    ASSERT_EQ(fix.executor.submit_calls, 1U);
    const uint64_t active_call = fix.executor.active->call_id;
    const std::size_t appends_before = fix.log_batches();

    ASSERT_EQ(fix.publish_obstacle_latest(1, 120), AYNI_OK);
    fix.step(1000); // Single step drains slot -> triggers reflex -> stops executor!
    EXPECT_EQ(fix.executor.cancel_calls, 1U);
    EXPECT_EQ(fix.executor.stop_calls, 1U);
    EXPECT_EQ(fix.log_batches(), appends_before);

    ayni_reflex_event_v1 ev{};
    ev.struct_size = sizeof(ev);
    ASSERT_EQ(ayni_take_reflex_event(fix.work.kernel(), &ev), AYNI_OK);
    EXPECT_EQ(ev.status, AYNI_OK);
    EXPECT_EQ(ev.action, AYNI_REFLEX_SOFT_STOP_RESOURCE);
    EXPECT_EQ(ev.affected_call_id, active_call);
    EXPECT_EQ(ev.spawned_call_id, 0U);
    EXPECT_EQ(ev.trigger_value, 120);
    EXPECT_EQ(std::string_view(ev.rule_id.data, ev.rule_id.size), "reflex.obstacle_stop");

    for (int i = 0; i < 5 && !call_finished; ++i) {
        fix.step();
    }
    ASSERT_TRUE(call_finished);
    EXPECT_EQ(call_res.outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(call_res.reason, AYNI_REASON_REFLEX);
    EXPECT_EQ(fix.resource_info().state, AYNI_RESOURCE_FREE);

    // Clear edge with safe reading (500 mm).
    ASSERT_EQ(fix.publish_obstacle_latest(2, 500), AYNI_OK);
    fix.step();

    // Subcase B: DEGRADED sample and disabled Scene do NOT fire reflex.
    fix.start_motor_goal({{30, 30, 1500}}, "run2");
    for (int i = 0; i < 8 && fix.executor.submit_calls < 2; ++i) {
        fix.step();
    }
    ASSERT_EQ(fix.executor.submit_calls, 2U);
    ASSERT_EQ(fix.publish_obstacle_latest(3, 80, AYNI_OBSERVATION_DEGRADED), AYNI_OK);
    fix.step();
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_stop").fire_count, 1U);
    EXPECT_EQ(fix.executor.cancel_calls, 1U);

    // Finish run2 cleanly, then verify disabled Scene gates off the reflex rule.
    fix.executor.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "ok", true);
    fix.step();
    fix.set_scene(false);
    ASSERT_EQ(fix.publish_obstacle_latest(4, 50, AYNI_OBSERVATION_GOOD), AYNI_OK);
    fix.step();
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_stop").fire_count, 1U);
}

// ADR-0112: reading a sample, firing a rule, and sending STOP each spend a
// separate item. Continued sensor updates must not delay an already requested stop.
TEST(ReflexAndSession, single_item_steps_resume_reflex_before_more_samples) {
    ReflexFixture fix;
    fix.extra_configure = [](ayni_kernel_t *kernel) {
        const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 200,
                                                 obstacle_validity_us};
        ayni_reflex_rule_v1 rule{};
        rule.struct_size = sizeof(rule);
        rule.version = 1;
        rule.rule_id = text("reflex.obstacle_stop");
        rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
        rule.trigger_mode = AYNI_REFLEX_TRIGGER_IN_RANGE;
        rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
        rule.condition_count = 1;
        rule.conditions = &cond;
        rule.resource_id = text("motor.actuator");
        ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);
    };
    fix.start();
    fix.grant_motor(agent_a);
    fix.set_scene(true);
    fix.start_motor_goal({{40, 40, 1500}}, "short_budget");
    for (unsigned tries = 0; tries < 8 && fix.executor.submit_calls == 0U; ++tries) {
        fix.step();
    }
    ASSERT_EQ(fix.executor.submit_calls, 1U);

    ASSERT_EQ(fix.publish_obstacle_latest(1, 120), AYNI_OK);
    auto first = fix.work.node().step(1);
    EXPECT_EQ(first.items_done, 1U);
    EXPECT_EQ(first.more_ready, 1U);
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_stop").fire_count, 0U);

    ASSERT_EQ(fix.publish_obstacle_latest(2, 110), AYNI_OK);
    auto reflex = fix.work.node().step(1);
    EXPECT_EQ(reflex.items_done, 1U);
    EXPECT_EQ(reflex.more_ready, 1U);
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_stop").fire_count, 1U);
    EXPECT_EQ(fix.executor.cancel_calls, 0U);
    EXPECT_EQ(fix.executor.stop_calls, 0U);

    ASSERT_EQ(fix.publish_obstacle_latest(3, 90), AYNI_OK);
    auto stop = fix.work.node().step(1);
    EXPECT_EQ(stop.items_done, 1U);
    EXPECT_EQ(fix.executor.stop_calls, 1U);
    EXPECT_EQ(fix.executor.cancel_calls, 0U);
}

// Test 3b (ADR-0112): Tier 2 DURABLE holder protection and stop-failure fail-closed quarantine.
TEST(ReflexAndSession, ReflexProtectsDurableHolderAndQuarantinesOnStopFailure) {
    struct DurableAwareExecutor : SessionReflexExecutor {
        static ayni_submit_result_t on_submit_any(void *ctx,
                                                  const ayni_provider_request_v1 *request,
                                                  ayni_provider_completion_t *completion) {
            auto &self = *static_cast<SessionReflexExecutor *>(ctx);
            ++self.submit_calls;
            if (request == nullptr || completion == nullptr) {
                return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
            }
            ayni_execution_permit_v1 permit{};
            if (request->execution_permit != nullptr) {
                permit = *request->execution_permit;
                self.last_permit = permit;
                self.last_permit_id = permit.permit_id;
                self.last_operation_id = permit.operation_id;
            }
            self.active = Active{
                request->attempt, request->logical_call_id, permit, completion, false, false};
            self.last_stopped = false;
            self.submitted_calls.push_back(request->logical_call_id);
            return AYNI_SUBMIT_ACCEPTED;
        }
    };

    ReflexFixture fix;
    fix.extra_configure = [&](ayni_kernel_t *kernel) {
        const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 200,
                                                 obstacle_validity_us};
        ayni_reflex_rule_v1 rule{};
        rule.struct_size = sizeof(rule);
        rule.version = 1;
        rule.rule_id = text("reflex.obstacle_stop");
        rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
        rule.trigger_mode = AYNI_REFLEX_TRIGGER_IN_RANGE;
        rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
        rule.condition_count = 1;
        rule.conditions = &cond;
        rule.resource_id = text("motor.actuator");
        ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);

        // Register an exclusive non-actuator resource + DURABLE call + second reflex rule
        // (catalogs::dispatchable forbids non-physical calls from holding AYNI_RESOURCE_ACTUATOR).
        const ayni_resource_desc_v1 slot_res{
            sizeof(slot_res), AYNI_RESOURCE_EXCLUSIVE, text("work.slot"), 1, 0, 0, 0, nullptr, {}};
        ASSERT_EQ(ayni_register_resource(kernel, &slot_res), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, text("work.slot"), AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        ayni_reflex_rule_v1 slot_rule{};
        slot_rule.struct_size = sizeof(slot_rule);
        slot_rule.version = 1;
        slot_rule.rule_id = text("reflex.slot_stop");
        slot_rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
        slot_rule.trigger_mode = AYNI_REFLEX_TRIGGER_IN_RANGE;
        slot_rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
        slot_rule.condition_count = 1;
        slot_rule.conditions = &cond;
        slot_rule.resource_id = text("work.slot");
        ASSERT_EQ(ayni_register_reflex_rule(kernel, &slot_rule), AYNI_OK);

        const ayni_call_resource_v1 req{text("work.slot"), 1, 0};
        ayni_capability_desc_v1 d_cap{sizeof(d_cap), 1, text("motor.durable"), 0, 0, nullptr};
        ASSERT_EQ(ayni_register_capability(kernel, &d_cap), AYNI_OK);
        ayni_schema_node_v1 text_schema{};
        text_schema.kind = AYNI_VALUE_TEXT;
        text_schema.subtree_end = 1;
        text_schema.max_count = 64;
        const ayni_business_contract_v1 text_business{
            {&text_schema, 1}, {&text_schema, 1}, 0, 0, nullptr};
        const ayni_call_capability_v1 d_call{
            sizeof(d_call),
            1,
            text("motor.durable"),
            {sizeof(ayni_call_contract_v1), AYNI_CALL_ACTION, AYNI_CALL_TOOL, 0,
             AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0, 5000,
             AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 64, 64, 1, &req, nullptr,
             AYNI_CALL_DURABILITY_DURABLE, 0},
            &text_business};
        ASSERT_EQ(ayni_register_call_capability(kernel, &d_call), AYNI_OK);
        const ayni_provider_v1 d_prov{sizeof(d_prov),
                                      1,
                                      text("durable.prov"),
                                      text("durable.ep"),
                                      {},
                                      &fix.executor,
                                      1,
                                      0,
                                      &d_call,
                                      DurableAwareExecutor::on_submit_any,
                                      SessionReflexExecutor::on_cancel,
                                      nullptr,
                                      nullptr};
        ASSERT_EQ(ayni_register_provider(kernel, &d_prov), AYNI_OK);
        const ayni_binding_v1 d_bind{sizeof(d_bind),
                                     1,
                                     text("motor.durable"),
                                     text("motor.durable"),
                                     1,
                                     1,
                                     text("durable.prov"),
                                     text("durable.ep"),
                                     {},
                                     0,
                                     0,
                                     nullptr};
        ASSERT_EQ(ayni_register_binding(kernel, &d_bind), AYNI_OK);
        auto d_goal = goal_type_desc("durable.goal", 1);
        d_goal.validate = [](void *, ayni_bytes_v1) { return AYNI_OK; };
        d_goal.export_requirements = [](void *, ayni_bytes_v1,
                                        const ayni_goal_requirement_sink_v1 *sink) {
            const ayni_goal_requirement_v1 need{
                sizeof(need), 1, text("motor.durable"), {sizeof(ayni_scope_v1), 0, nullptr}};
            return sink->emit(sink->ctx, &need);
        };
        ASSERT_EQ(ayni_register_goal_type(kernel, &d_goal), AYNI_OK);
    };
    fix.start();
    fix.grant_motor(agent_a);
    fix.set_scene(true);

    // Grant motor.durable to agent_a.
    {
        ayni_grant_create_v1 create{};
        create.struct_size = sizeof(create);
        create.capability_version = 1;
        create.capability_id = text("motor.durable");
        create.subject_instance_id = fix.work.instance(agent_a);
        create.lifetime = AYNI_GRANT_PERMANENT;
        create.scope = {sizeof(ayni_scope_v1), 0, nullptr};
        auto intent = fix.work.intent(AYNI_INTENT_GRANT_CREATE, &create);
        intent.command_id = ++fix.command_id;
        ASSERT_EQ(fix.work.submit(intent).status, AYNI_OK);
        fix.flush_durable();
    }

    // Start Tier 2 DURABLE call holding motor.actuator.
    fix.work.strategy().answer = [](const ayni_task_snapshot_v1 &snap, ayni_policy_output_v1 &out) {
        ayni_task_call_result_v1 prior{};
        prior.struct_size = sizeof(prior);
        if (snap.call_result(snap.call_context, text("dur1"), &prior) == AYNI_OK) {
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        }
        ayni_value_node_v1 node{};
        node.kind = AYNI_VALUE_TEXT;
        node.subtree_end = 1;
        node.value.text = text("cal");
        ayni_call_request_v1 req{};
        req.struct_size = sizeof(req);
        req.kind = AYNI_POLICY_REQUEST_CALL;
        req.binding_version = 1;
        req.step_key = text("dur1");
        req.binding_id = text("motor.durable");
        req.value = {&node, 1};
        req.mode = AYNI_CALL_MODE_FOREGROUND;
        return out.request_call(out.call_context, &req);
    };
    ASSERT_EQ(fix.work
                  .create(WorkNode::create_body(fix.work.instance(agent_a), 0, "durable.goal", 1,
                                                {}, AYNI_TRIGGER_ONCE))
                  .status,
              AYNI_OK);
    // DURABLE Goal + Task + Call require three storage_write_interval_us (100 ms) flushes.
    for (int i = 0; i < 50 && fix.executor.submit_calls == 0; ++i) {
        fix.step();
    }
    ASSERT_EQ(fix.executor.submit_calls, 1U);

    // Obstacle arrives -> DURABLE call must NOT be stopped; reflex records AYNI_DENIED!
    ASSERT_EQ(fix.publish_obstacle_latest(1, 50), AYNI_OK);
    fix.step();
    EXPECT_EQ(fix.executor.cancel_calls, 0U);
    EXPECT_EQ(fix.reflex_info("reflex.slot_stop").denied_count, 1U);
    ayni_reflex_event_v1 ev{};
    ev.struct_size = sizeof(ev);
    ASSERT_EQ(ayni_take_reflex_event(fix.work.kernel(), &ev), AYNI_OK);
    EXPECT_EQ(ev.status, AYNI_DENIED);
    EXPECT_EQ(std::string_view(ev.rule_id.data, ev.rule_id.size), "reflex.slot_stop");

    // Finish DURABLE call and clear edge.
    fix.executor.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "ok",
                               false);
    fix.step();
    ASSERT_EQ(fix.publish_obstacle_latest(2, 600), AYNI_OK);
    fix.step();

    // Stop-failure fail-closed quarantine: fail_stop=true on EPHEMERAL motor.run call.
    fix.executor.fail_stop = true;
    fix.start_motor_goal({{30, 30, 1500}}, "q1");
    for (int i = 0; i < 8 && fix.executor.submit_calls < 2; ++i) {
        fix.step();
    }
    ASSERT_EQ(fix.executor.submit_calls, 2U);
    ASSERT_EQ(fix.publish_obstacle_latest(3, 80), AYNI_OK);
    for (int i = 0; i < 12 && fix.resource_info().state != AYNI_RESOURCE_QUARANTINED; ++i) {
        fix.step(session_poll_us);
    }
    EXPECT_EQ(fix.resource_info().state, AYNI_RESOURCE_QUARANTINED);
    // Allow hardware stop readback during teardown so DRAINING reaches STOPPED cleanly.
    fix.executor.fail_stop = false;
    if (fix.executor.active) {
        fix.executor.active->stopped = true;
    }
    fix.executor.last_stopped = true;
}

// Test 4 (ADR-0112): STALE_OR_GAP fail-safe stops active EPHEMERAL actuator calls automatically
// when a sensor crosses max_age_us (via report.next_deadline_us) OR on an explicit gap, while
// remaining silent when the actuator is idle.
TEST(ReflexAndSession, ReflexStaleOrGapFailSafe) {
    constexpr uint64_t watchdog_max_age_us = 150000;
    ReflexFixture fix;
    fix.extra_configure = [](ayni_kernel_t *kernel) {
        const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 10000,
                                                 watchdog_max_age_us};
        ayni_reflex_rule_v1 rule{};
        rule.struct_size = sizeof(rule);
        rule.version = 1;
        rule.rule_id = text("reflex.obstacle_watchdog");
        rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
        rule.trigger_mode = AYNI_REFLEX_TRIGGER_STALE_OR_GAP;
        rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
        rule.condition_count = 1;
        rule.conditions = &cond;
        rule.resource_id = text("motor.actuator");
        ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);
    };
    fix.start();
    fix.grant_motor(agent_a);
    fix.set_scene(true);

    // Idle actuator with absent sensor must not fire fail-safe.
    fix.step();
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_watchdog").fire_count, 0U);

    // Publish fresh reading and start EPHEMERAL motor call.
    const uint64_t capture_us = fix.now_us;
    ASSERT_EQ(fix.publish_obstacle_latest(1, 800, AYNI_OBSERVATION_GOOD, capture_us), AYNI_OK);
    fix.start_motor_goal({{35, 35, 1500}}, "wd1");
    ayni_step_report_v1 report{};
    for (int i = 0; i < 8 && fix.executor.submit_calls == 0; ++i) {
        report = fix.step(1000);
    }
    ASSERT_EQ(fix.executor.submit_calls, 1U);
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_watchdog").fire_count, 0U);
    // Core must schedule owner wakeup no later than capture_us + watchdog_max_age_us + 1.
    EXPECT_LE(report.next_deadline_us, capture_us + watchdog_max_age_us + 1U);

    // Jump straight to staleness expiry -> fail-safe fires automatically!
    const uint64_t stale_at = capture_us + watchdog_max_age_us + 1U;
    fix.step(stale_at > fix.now_us ? stale_at - fix.now_us : 0U);
    EXPECT_EQ(fix.executor.cancel_calls, 1U);
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_watchdog").fire_count, 1U);
    ayni_reflex_event_v1 ev{};
    ev.struct_size = sizeof(ev);
    ASSERT_EQ(ayni_take_reflex_event(fix.work.kernel(), &ev), AYNI_OK);
    EXPECT_EQ(ev.status, AYNI_OK);
    EXPECT_EQ(std::string_view(ev.trigger_source.data, ev.trigger_source.size),
              "sensor.obstacle_mm");

    fix.step();
    EXPECT_EQ(fix.resource_info().state, AYNI_RESOURCE_FREE);

    // Second half: explicit observation gap invalidating the fresh sample triggers fail-safe.
    ASSERT_EQ(fix.publish_obstacle_latest(2, 900), AYNI_OK);
    fix.start_motor_goal({{30, 30, 1500}}, "wd2");
    for (int i = 0; i < 8 && fix.executor.submit_calls < 2; ++i) {
        fix.step(1000);
    }
    ASSERT_EQ(fix.executor.submit_calls, 2U);
    const ayni_observation_gap_v1 gap{
        sizeof(gap), 1, fix.work.node().info().boot_epoch, text("sensor.obstacle_mm"), 1, 2};
    ASSERT_EQ(ayni_report_observation_gap(fix.work.kernel(), &gap), AYNI_OK);
    fix.step(1000);
    EXPECT_EQ(fix.executor.cancel_calls, 2U);
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_watchdog").fire_count, 2U);
}

// Test 5 (ADR-0112): PREEMPT_CALL dispatches a zero-LLM, zero-WAL deterministic EPHEMERAL Call
// that supersedes the active holder, recycles its CallTable slot on completion, obeys edge latch
// + cooldown, and fails closed on revoked Grant, narrow Scope, paused Agent, or disabled Scene.
TEST(ReflexAndSession, ReflexPreemptCallZeroLlmZeroWalAndAuthorityEnforcement) {
    constexpr uint64_t cooldown_us = 300000;
    const auto backup_nodes = make_segments_value({{-30, -30, 200}});
    ReflexFixture fix;
    fix.extra_configure = [&](ayni_kernel_t *kernel) {
        const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 150,
                                                 obstacle_validity_us};
        ayni_reflex_rule_v1 rule{};
        rule.struct_size = sizeof(rule);
        rule.version = 1;
        rule.rule_id = text("reflex.backup");
        rule.action = AYNI_REFLEX_PREEMPT_CALL;
        rule.trigger_mode = AYNI_REFLEX_TRIGGER_IN_RANGE;
        rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
        rule.priority = 200;
        rule.cooldown_us = cooldown_us;
        rule.scene_id = text("motor.manual-bench");
        rule.scene_version = 1;
        rule.condition_count = 1;
        rule.conditions = &cond;
        rule.owner_agent_id = agent_a;
        rule.binding_id = text("motor.run");
        rule.binding_version = 3;
        rule.input = {backup_nodes.data(), static_cast<uint32_t>(backup_nodes.size())};
        ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);
    };
    fix.start();
    const uint64_t grant_id = fix.grant_motor(agent_a, -60, 60, 5000);
    fix.set_scene(true);

    // Start foreground motor call (40, 40, 2000).
    fix.start_motor_goal({{40, 40, 2000}}, "fwd1");
    for (int i = 0; i < 8 && fix.executor.submit_calls == 0; ++i) {
        fix.step();
    }
    ASSERT_EQ(fix.executor.submit_calls, 1U);
    const std::size_t decides_before = fix.work.strategy().seen.size();
    const std::size_t appends_before = fix.log_batches();

    // Obstacle triggers PREEMPT_CALL -> supersedes fwd1 and dispatches (-30, -30, 200) directly!
    ASSERT_EQ(fix.publish_obstacle_latest(1, 100), AYNI_OK);
    for (int i = 0; i < 8 && fix.executor.submit_calls < 2; ++i) {
        fix.step(1000);
    }
    ASSERT_EQ(fix.executor.submit_calls, 2U);
    EXPECT_EQ(fix.executor.cancel_calls, 1U);
    const auto *m1_range = find_envelope_field(fix.executor.last_permit.envelope, "motor1_percent");
    const auto *dur_range = find_envelope_field(fix.executor.last_permit.envelope, "duration_ms");
    ASSERT_NE(m1_range, nullptr);
    ASSERT_NE(dur_range, nullptr);
    EXPECT_EQ(m1_range->minimum, -30);
    EXPECT_EQ(m1_range->maximum, -30);
    EXPECT_EQ(dur_range->minimum, 200);
    EXPECT_EQ(fix.log_batches(), appends_before);

    ayni_reflex_event_v1 ev{};
    ev.struct_size = sizeof(ev);
    ASSERT_EQ(ayni_take_reflex_event(fix.work.kernel(), &ev), AYNI_OK);
    EXPECT_EQ(ev.status, AYNI_OK);
    EXPECT_EQ(ev.action, AYNI_REFLEX_PREEMPT_CALL);
    EXPECT_NE(ev.spawned_call_id, 0U);
    EXPECT_EQ(fix.reflex_info("reflex.backup").active_call_id, ev.spawned_call_id);

    // Complete reflex call -> slot recycled, active_call_id cleared, resource FREE.
    fix.executor.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "backed",
                               true);
    fix.step();
    EXPECT_EQ(fix.reflex_info("reflex.backup").active_call_id, 0U);
    EXPECT_EQ(fix.resource_info().state, AYNI_RESOURCE_FREE);
    // Note: original task's strategy may run once after fwd1's cancellation, so verify 0 extra
    // decides occurred specifically on the reflex fire path during the second trigger below!
    (void)decides_before;

    // EDGE_TRIGGERED latch: staying in range does NOT re-fire.
    ASSERT_EQ(fix.publish_obstacle_latest(2, 90), AYNI_OK);
    fix.step(50000);
    EXPECT_EQ(fix.reflex_info("reflex.backup").fire_count, 1U);

    // Clear edge, re-enter BEFORE cooldown expires -> suppressed.
    ASSERT_EQ(fix.publish_obstacle_latest(3, 500), AYNI_OK);
    fix.step(50000);
    ASSERT_EQ(fix.publish_obstacle_latest(4, 80), AYNI_OK);
    fix.step(50000);
    EXPECT_EQ(fix.reflex_info("reflex.backup").fire_count, 1U);

    // Advance past cooldown -> fires cleanly a second time with 0 strategy decides & 0 WAL!
    ASSERT_EQ(fix.publish_obstacle_latest(5, 500), AYNI_OK);
    fix.step(cooldown_us);
    const std::size_t decides_idle = fix.work.strategy().seen.size();
    const std::size_t appends_idle = fix.log_batches();
    ASSERT_EQ(fix.publish_obstacle_latest(6, 70), AYNI_OK);
    for (int i = 0; i < 6 && fix.executor.submit_calls < 3; ++i) {
        fix.step(1000);
    }
    EXPECT_EQ(fix.executor.submit_calls, 3U);
    EXPECT_EQ(fix.reflex_info("reflex.backup").fire_count, 2U);
    EXPECT_EQ(fix.work.strategy().seen.size(), decides_idle);
    EXPECT_EQ(fix.log_batches(), appends_idle);
    fix.executor.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "backed",
                               true);
    fix.step();

    // Authority enforcement 1: revoked Grant -> refused with AYNI_DENIED, no submit!
    fix.revoke_grant(grant_id);
    ASSERT_EQ(fix.publish_obstacle_latest(7, 500), AYNI_OK);
    fix.step(cooldown_us);
    ASSERT_EQ(fix.publish_obstacle_latest(8, 60), AYNI_OK);
    fix.step();
    EXPECT_EQ(fix.executor.submit_calls, 3U);
    EXPECT_EQ(fix.reflex_info("reflex.backup").denied_count, 1U);

    // Authority enforcement 2: Grant whose scope [0, 50] excludes reflex input (-30) -> refused!
    const uint64_t narrow_grant = fix.grant_motor(agent_a, 0, 50, 5000);
    ASSERT_EQ(fix.publish_obstacle_latest(9, 500), AYNI_OK);
    fix.step(cooldown_us);
    ASSERT_EQ(fix.publish_obstacle_latest(10, 60), AYNI_OK);
    fix.step();
    EXPECT_EQ(fix.executor.submit_calls, 3U);
    EXPECT_EQ(fix.reflex_info("reflex.backup").denied_count, 2U);

    // Authority enforcement 3: valid Grant but paused Agent -> refused!
    fix.revoke_grant(narrow_grant);
    fix.grant_motor(agent_a, -60, 60, 5000);
    {
        ayni_agent_info_v1 ainfo{};
        ainfo.struct_size = sizeof(ainfo);
        ASSERT_EQ(ayni_get_agent(fix.work.kernel(), agent_a, &ainfo), AYNI_OK);
        const ayni_instance_target_v1 target{sizeof(target), 0, ainfo.instance_id};
        auto intent = fix.work.intent(AYNI_INTENT_INSTANCE_PAUSE, &target, ainfo.revision);
        intent.command_id = ++fix.command_id;
        ASSERT_EQ(fix.work.submit(intent).status, AYNI_OK);
        fix.flush_durable();
    }
    ASSERT_EQ(fix.publish_obstacle_latest(11, 500), AYNI_OK);
    fix.step(cooldown_us);
    ASSERT_EQ(fix.publish_obstacle_latest(12, 60), AYNI_OK);
    fix.step();
    EXPECT_EQ(fix.executor.submit_calls, 3U);
    EXPECT_EQ(fix.reflex_info("reflex.backup").denied_count, 3U);
}

// Test 6 (ADR-0112): ayni_set_reflex_enabled() toggles rules in RAM with 0 WAL writes,
// and rebooting on the exact schema 11 storage volume restores RUNNING with volatile toggle reset
// to AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT.
TEST(ReflexAndSession, ReflexRuntimeToggleAndSchema11Compatibility) {
    EXPECT_EQ(AYNI_STORAGE_SCHEMA_CURRENT, 11U);
    ReflexFixture fix;
    fix.extra_configure = [](ayni_kernel_t *kernel) {
        const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 200,
                                                 obstacle_validity_us};
        ayni_reflex_rule_v1 rule{};
        rule.struct_size = sizeof(rule);
        rule.version = 1;
        rule.rule_id = text("reflex.obstacle_stop");
        rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
        rule.trigger_mode = AYNI_REFLEX_TRIGGER_IN_RANGE;
        rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT;
        rule.condition_count = 1;
        rule.conditions = &cond;
        rule.resource_id = text("motor.actuator");
        ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);
    };
    fix.start();
    fix.grant_motor(agent_a);
    fix.set_scene(true);

    // Disable reflex in RAM -> 0 WAL writes, and obstacle does NOT stop running call.
    const std::size_t appends_before = fix.log_batches();
    ASSERT_EQ(ayni_set_reflex_enabled(fix.work.kernel(), text("reflex.obstacle_stop"), 0U),
              AYNI_OK);
    EXPECT_EQ(fix.log_batches(), appends_before);
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_stop").enabled, 0U);

    fix.start_motor_goal({{30, 30, 1500}}, "tog1");
    for (int i = 0; i < 8 && fix.executor.submit_calls == 0; ++i) {
        fix.step();
    }
    ASSERT_EQ(fix.executor.submit_calls, 1U);
    ASSERT_EQ(fix.publish_obstacle_latest(1, 50), AYNI_OK);
    fix.step();
    EXPECT_EQ(fix.executor.cancel_calls, 0U);

    // Re-enable in RAM (0 WAL) -> fires immediately on the next step using the live sample!
    const std::size_t appends_mid = fix.log_batches();
    ASSERT_EQ(ayni_set_reflex_enabled(fix.work.kernel(), text("reflex.obstacle_stop"), 1U),
              AYNI_OK);
    EXPECT_EQ(fix.log_batches(), appends_mid);
    fix.step();
    EXPECT_EQ(fix.executor.cancel_calls, 1U);
    EXPECT_EQ(fix.reflex_info("reflex.obstacle_stop").fire_count, 1U);

    // Disable again before clean shutdown, then reboot on the same schema 11 storage volume.
    ASSERT_EQ(ayni_set_reflex_enabled(fix.work.kernel(), text("reflex.obstacle_stop"), 0U),
              AYNI_OK);
    fix.work.node().reboot();
    fix.work.node().recover();
    fix.work.reopen_session();
    EXPECT_EQ(fix.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const auto after_boot = fix.reflex_info("reflex.obstacle_stop");
    EXPECT_EQ(after_boot.enabled, 1U);
    EXPECT_EQ(after_boot.fire_count, 0U);
}

// Test 7 (ADR-0112): STALE_OR_GAP rules in cooldown or EDGE_TRIGGERED latch must not report
// next_deadline_us <= now_us / more_ready == 1 while an EPHEMERAL holder remains active.
TEST(ReflexAndSession, StaleOrGapCooldownAndEdgeLatchDoNotBusySpinOwner) {
    constexpr uint64_t cooldown_us = 250000;
    // Subcase A: cooldown_us clamps next_deadline_us until cooldown expires.
    {
        ReflexFixture fix;
        fix.executor.auto_cancel_with_proof = false; // Keep holder active after soft-stop signal.
        fix.extra_configure = [](ayni_kernel_t *kernel) {
            const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 0, 50000};
            ayni_reflex_rule_v1 rule{};
            rule.struct_size = sizeof(rule);
            rule.version = 1;
            rule.rule_id = text("reflex.stale_cool");
            rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
            rule.trigger_mode = AYNI_REFLEX_TRIGGER_STALE_OR_GAP;
            rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT;
            rule.cooldown_us = cooldown_us;
            rule.condition_count = 1;
            rule.conditions = &cond;
            rule.resource_id = text("motor.actuator");
            ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);
        };
        fix.start();
        fix.grant_motor(agent_a);
        fix.set_scene(true);
        // No obstacle sample published -> immediately stale/missing.
        fix.start_motor_goal({{30, 30, 1500}}, "cool1");
        for (int i = 0; i < 12 && fix.reflex_info("reflex.stale_cool").fire_count == 0U; ++i) {
            fix.step();
        }
        ASSERT_EQ(fix.reflex_info("reflex.stale_cool").fire_count, 1U);
        const uint64_t fired_at = fix.reflex_info("reflex.stale_cool").last_fired_us;
        // Drain any remaining transport work, then verify owner is NOT stuck busy-spinning.
        const auto idle_report = fix.step(1000);
        EXPECT_EQ(idle_report.more_ready, 0U);
        EXPECT_GE(idle_report.next_deadline_us, fired_at + cooldown_us);
    }

    // Subcase B: EDGE_TRIGGERED latched_active does not schedule immediate STALE_OR_GAP wakeups.
    {
        ReflexFixture fix;
        fix.executor.auto_cancel_with_proof = false;
        fix.extra_configure = [](ayni_kernel_t *kernel) {
            const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 0, 50000};
            ayni_reflex_rule_v1 rule{};
            rule.struct_size = sizeof(rule);
            rule.version = 1;
            rule.rule_id = text("reflex.stale_edge");
            rule.action = AYNI_REFLEX_SOFT_STOP_RESOURCE;
            rule.trigger_mode = AYNI_REFLEX_TRIGGER_STALE_OR_GAP;
            rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
            rule.condition_count = 1;
            rule.conditions = &cond;
            rule.resource_id = text("motor.actuator");
            ASSERT_EQ(ayni_register_reflex_rule(kernel, &rule), AYNI_OK);
        };
        fix.start();
        fix.grant_motor(agent_a);
        fix.set_scene(true);
        fix.start_motor_goal({{30, 30, 1500}}, "edge1");
        for (int i = 0; i < 12 && fix.reflex_info("reflex.stale_edge").fire_count == 0U; ++i) {
            fix.step();
        }
        ASSERT_EQ(fix.reflex_info("reflex.stale_edge").fire_count, 1U);
        const auto idle_report = fix.step(1000);
        EXPECT_EQ(idle_report.more_ready, 0U);
        EXPECT_GT(idle_report.next_deadline_us, fix.now_us);
    }
}

// ADR-0114: exact full-product division precedes saturation, including 32-bit-compatible edges.
TEST(ReflexParameters, AffineIntegerEdgesSaturateWithoutPrematureProductTruncation) {
    using ayni::foundation::map_reflex_i64;
    EXPECT_EQ(map_reflex_i64(INT64_MAX, INT64_MIN, INT64_MAX, 2, 2, 0, INT64_MIN, INT64_MAX),
              INT64_MAX);
    EXPECT_EQ(map_reflex_i64(INT64_MIN, INT64_MIN, INT64_MAX, -1, 1, 0, INT64_MIN, INT64_MAX),
              INT64_MAX);
    EXPECT_EQ(map_reflex_i64(INT64_MIN, INT64_MIN, INT64_MAX, 3, 3, 0, INT64_MIN, INT64_MAX),
              INT64_MIN);
    EXPECT_EQ(map_reflex_i64(INT64_MAX, INT64_MIN, INT64_MAX, INT64_MAX, 1, 99, -90, 90), 90);
    EXPECT_EQ(map_reflex_i64(-99, -10, 10, 3, 2, -2, -100, 100), -17);
    EXPECT_EQ(
        map_reflex_i64(INT64_MIN, INT64_MIN, INT64_MAX, 1, 1, INT64_MIN, INT64_MIN, INT64_MAX),
        INT64_MIN);
}

// ADR-0114: nested object path is resolved once; patched bytes still undergo full Grant scope
// preflight, with refusal audited and zero Provider execution outside the approved range.
void dynamic_parameter_case(unsigned variant) {
    struct Receiver {
        int64_t value = 0;
        uint32_t calls = 0;
        static ayni_submit_result_t submit(void *context, const ayni_provider_request_v1 *request,
                                           ayni_provider_completion_t *completion) {
            auto &self = *static_cast<Receiver *>(context);
            ++self.calls;
            self.value = request->value.nodes[2].value.integer;
            ayni_provider_result_v1 result{};
            result.struct_size = sizeof(result);
            result.attempt = request->attempt;
            result.outcome = AYNI_CALL_COMPLETED;
            result.status = AYNI_OK;
            result.effect = AYNI_CALL_EFFECT_COMPLETE;
            result.result = text("done");
            return ayni_provider_post(completion, &result) == AYNI_OK
                       ? AYNI_SUBMIT_ACCEPTED
                       : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
    } receiver;
    ReflexFixture fix;
    fix.work.node().config().audit_capacity = 32;
    fix.extra_configure = [&](ayni_kernel_t *kernel) {
        ayni_authority_field_desc_v1 field{};
        field.struct_size = sizeof(field);
        field.field_id = 1;
        field.usage = AYNI_AUTHORITY_PARAMETER;
        field.domain.struct_size = sizeof(field.domain);
        field.domain.kind = AYNI_AUTHORITY_I64;
        field.domain.unit = text("percent");
        field.domain.values.integer = {-90, 90};
        const ayni_capability_desc_v1 cap{sizeof(cap), 1, text("parameter.apply"), 1, 0, &field};
        ASSERT_EQ(ayni_register_capability(kernel, &cap), AYNI_OK);
        std::array<ayni_schema_node_v1, 3> schema{};
        schema[0].kind = AYNI_VALUE_OBJECT;
        schema[0].subtree_end = 3;
        schema[1].kind = AYNI_VALUE_OBJECT;
        schema[1].field_id = 10;
        schema[1].subtree_end = 3;
        schema[1].flags = AYNI_SCHEMA_REQUIRED;
        schema[1].name = text("target");
        schema[2].kind = AYNI_VALUE_I64;
        schema[2].field_id = 1;
        schema[2].subtree_end = 3;
        schema[2].flags = AYNI_SCHEMA_REQUIRED;
        schema[2].name = text("percent");
        schema[2].unit = text("percent");
        schema[2].min_i64 = -90;
        schema[2].max_i64 = 90;
        ayni_schema_node_v1 result{};
        result.kind = AYNI_VALUE_TEXT;
        result.subtree_end = 1;
        result.max_count = 32;
        ayni_authority_projection_v1 projection{};
        projection.field_id = 1;
        projection.path_count = 2;
        projection.path[0] = 10;
        projection.path[1] = 1;
        projection.unit = text("percent");
        const ayni_business_contract_v1 business{
            {schema.data(), 3}, {&result, 1}, 1, 0, &projection};
        const ayni_call_capability_v1 call{sizeof(call),
                                           1,
                                           text("parameter.apply"),
                                           {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY,
                                            AYNI_CALL_TOOL, 0, AYNI_CALL_IDEMPOTENT,
                                            AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_NONE, 0, 1000,
                                            AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 32,
                                            0, nullptr, nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                                           &business};
        ASSERT_EQ(ayni_register_call_capability(kernel, &call), AYNI_OK);
        const ayni_provider_v1 provider{sizeof(provider),
                                        1,
                                        text("parameter.provider"),
                                        text("parameter.endpoint"),
                                        {},
                                        &receiver,
                                        1,
                                        0,
                                        &call,
                                        Receiver::submit,
                                        nullptr,
                                        nullptr,
                                        nullptr};
        ASSERT_EQ(ayni_register_provider(kernel, &provider), AYNI_OK);
        const ayni_binding_v1 binding{sizeof(binding),
                                      1,
                                      text("parameter.apply"),
                                      text("parameter.apply"),
                                      1,
                                      1,
                                      provider.provider_id,
                                      provider.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        ASSERT_EQ(ayni_register_binding(kernel, &binding), AYNI_OK);
        std::array<ayni_value_node_v1, 3> input{};
        input[0].kind = AYNI_VALUE_OBJECT;
        input[0].subtree_end = 3;
        input[1].kind = AYNI_VALUE_OBJECT;
        input[1].field_id = 10;
        input[1].subtree_end = 3;
        input[2].kind = AYNI_VALUE_I64;
        input[2].field_id = 1;
        input[2].subtree_end = 3;
        ayni_reflex_param_binding_v1 parameter{};
        parameter.path_count = 2;
        parameter.path[0] = 10;
        parameter.path[1] = 1;
        parameter.input_min = 0;
        parameter.input_max = 200;
        parameter.scale_num = -1;
        parameter.scale_den = 2;
        parameter.offset = 100;
        parameter.output_min = -90;
        parameter.output_max = 90;
        const ayni_observation_condition_v1 condition{text("sensor.obstacle_mm"), 1, 0, 0, 200,
                                                      obstacle_validity_us};
        ayni_reflex_rule_v1 rule{};
        rule.struct_size = sizeof(rule);
        rule.version = 1;
        rule.rule_id = text("reflex.dynamic");
        rule.action = AYNI_REFLEX_PREEMPT_CALL;
        rule.trigger_mode = AYNI_REFLEX_TRIGGER_IN_RANGE;
        rule.flags = AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT | AYNI_REFLEX_FLAG_EDGE_TRIGGERED;
        rule.condition_count = 1;
        rule.conditions = &condition;
        rule.owner_agent_id = agent_a;
        rule.binding_id = binding.binding_id;
        rule.binding_version = 1;
        rule.input = {input.data(), 3};
        if (variant == 1) {
            parameter.scale_den = 0;
        } else if (variant == 3) {
            parameter.path[1] = 99; // Missing business path.
        } else if (variant == 4) {
            parameter.path_count = 1; // Existing object is not an I64 payload.
            parameter.path[1] = 0;
        } else if (variant == 5) {
            parameter.output_max = 91; // Entire interval must be inside schema [-90,90].
        }
        const std::array<ayni_reflex_param_binding_v1, 2> parameters{parameter, parameter};
        rule.param_binding_count = variant == 2 ? 2U : 1U;
        rule.param_bindings = parameters.data();
        EXPECT_EQ(ayni_register_reflex_rule(kernel, &rule),
                  variant == 1 ? AYNI_INVALID_ARGUMENT : AYNI_OK);
    };
    if (variant >= 2) {
        // ADR-0114: cross-catalog path/type/interval/duplicate checks happen before RUNNING.
        fix.configure_defaults();
        fix.work.additional_registration = [&](ayni_kernel_t *kernel) {
            fix.register_motor_stack(kernel);
            fix.extra_configure(kernel);
        };
        fix.work.node().create();
        EXPECT_EQ(ayni_start(fix.work.kernel()), AYNI_INVALID_ARGUMENT);
        return;
    }
    fix.start();
    if (variant == 1) {
        return; // Structurally invalid parameter was refused without consuming a rule slot.
    }
    ayni_scope_field_v1 allowed{};
    allowed.struct_size = sizeof(allowed);
    allowed.field_id = 1;
    allowed.allowed.struct_size = sizeof(allowed.allowed);
    allowed.allowed.kind = AYNI_AUTHORITY_I64;
    allowed.allowed.unit = text("percent");
    allowed.allowed.values.integer = {-50, 50};
    const ayni_grant_create_v1 grant{sizeof(grant),
                                     1,
                                     text("parameter.apply"),
                                     fix.work.instance(agent_a),
                                     0,
                                     AYNI_GRANT_PERMANENT,
                                     0,
                                     0,
                                     {sizeof(ayni_scope_v1), 1, &allowed}};
    auto intent = fix.work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
    intent.command_id = ++fix.command_id;
    ASSERT_EQ(fix.work.submit(intent).status, AYNI_OK);
    fix.flush_durable();
    ASSERT_EQ(fix.publish_obstacle_latest(1, 120), AYNI_OK);
    for (unsigned at = 0; at < 8 && !receiver.calls; ++at) {
        fix.step(1000);
    }
    ASSERT_EQ(receiver.calls, 1U);
    EXPECT_EQ(receiver.value, 40);
    fix.step(1000);
    ASSERT_EQ(fix.publish_obstacle_latest(2, 400), AYNI_OK);
    fix.step(1000);
    ASSERT_EQ(fix.publish_obstacle_latest(3, 80), AYNI_OK);
    fix.step(1000);
    EXPECT_EQ(receiver.calls, 1U);
    EXPECT_EQ(fix.reflex_info("reflex.dynamic").denied_count, 1U);
    std::array<ayni_audit_record_v1, 32> records{};
    uint32_t count = 0;
    ASSERT_EQ(ayni_take_audit_records(fix.work.kernel(), records.data(), uint32_t(records.size()),
                                      &count),
              AYNI_OK);
    bool denied = false;
    for (uint32_t at = 0; at < count; ++at) {
        denied = denied ||
                 (records[at].kind == AYNI_AUDIT_REFLEX_EVENT && records[at].status != AYNI_OK &&
                  (records[at].flags & AYNI_AUDIT_FLAG_DENIED));
    }
    EXPECT_TRUE(denied);
    ayni::test::LatencySamples samples;
    const auto appends_before = fix.log_batches();
    uint64_t sequence = 3;
    for (unsigned at = 0; at < 256; ++at) {
        ASSERT_EQ(fix.publish_obstacle_latest(++sequence, 400), AYNI_OK);
        fix.step(0); // Reset edge latch and drain the preceding terminal result.
        const auto calls_before = receiver.calls;
        const auto wakes_before = fix.work.node().sim().wake_count();
        const auto started = ayni::test::LatencySamples::Clock::now();
        ASSERT_EQ(fix.publish_obstacle_latest(++sequence, 120), AYNI_OK);
        EXPECT_GT(fix.work.node().sim().wake_count(), wakes_before);
        for (unsigned step = 0; step < 8 && receiver.calls == calls_before; ++step) {
            fix.step(0);
        }
        samples.add(started);
        ASSERT_EQ(receiver.calls, calls_before + 1);
        ASSERT_EQ(receiver.value, 40);
    }
    EXPECT_EQ(fix.log_batches(), appends_before);
    samples.print("ObservationSlot->parameterized_Reflex->Provider/zero_WAL");
    EXPECT_LT(samples.percentile(99), 300);
    EXPECT_LT(samples.percentile(100), 2000);
}
TEST(ReflexParameters, DynamicProjectionRechecksScopeAndMeetsHostLatencyBudget) {
    dynamic_parameter_case(0);
}
TEST(ReflexParameters, ZeroDenominatorIsRefusedAtRegistration) {
    dynamic_parameter_case(1);
}
TEST(ReflexParameters, DuplicatePayloadOffsetIsRefusedAtStartup) {
    dynamic_parameter_case(2);
}
TEST(ReflexParameters, MissingBusinessPathIsRefusedAtStartup) {
    dynamic_parameter_case(3);
}
TEST(ReflexParameters, NonIntegerPayloadIsRefusedAtStartup) {
    dynamic_parameter_case(4);
}
TEST(ReflexParameters, OutputIntervalOutsideSchemaIsRefusedAtStartup) {
    dynamic_parameter_case(5);
}

} // namespace
