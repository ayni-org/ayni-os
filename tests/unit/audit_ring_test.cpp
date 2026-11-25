// ADR-0113: Phase 4 Core AuditRing verification (ABI 0.60, schema 11 unchanged):
// 1. All 7 audit event kinds (LIFECYCLE, CALL_TERMINAL EPHEMERAL/DURABLE, AUTHORITY_DENIED,
//    RESOURCE_PREEMPTED, ACTUATOR_QUARANTINE, ACTUATOR_RELEASE, REFLEX_EVENT).
// 2. Fixed-capacity overwrite-oldest ring overflow (dropped_overflow + monotonic seq gaps),
//    zero WAL impact, disabled capacity=0 zero overhead, and argument/callback guards.

#include "work_node.h"

#include <ayni/audit.h>
#include <ayni/calls.h>
#include <ayni/execution.h>
#include <ayni/observations.h>
#include <ayni/reflex.h>
#include <ayni/scenes.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ayni::test;

constexpr uint64_t step_tick_us = 10000;
constexpr uint64_t control_domain = 91;
constexpr uint32_t lease_us = 200000;
constexpr uint32_t poll_us = 20000;
constexpr uint64_t obstacle_validity_us = 500000;

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

std::vector<ayni_value_node_v1> make_segment(int64_t m1, int64_t m2, int64_t ms) {
    std::vector<ayni_value_node_v1> nodes(6);
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = 6;
    nodes[1].kind = AYNI_VALUE_LIST;
    nodes[1].field_id = 1;
    nodes[1].subtree_end = 6;
    nodes[2].kind = AYNI_VALUE_OBJECT;
    nodes[2].subtree_end = 6;
    nodes[3].kind = AYNI_VALUE_I64;
    nodes[3].field_id = 1;
    nodes[3].subtree_end = 4;
    nodes[3].value.integer = m1;
    nodes[4].kind = AYNI_VALUE_I64;
    nodes[4].field_id = 2;
    nodes[4].subtree_end = 5;
    nodes[4].value.integer = m2;
    nodes[5].kind = AYNI_VALUE_I64;
    nodes[5].field_id = 3;
    nodes[5].subtree_end = 6;
    nodes[5].value.integer = ms;
    return nodes;
}

struct AuditFixture {
    WorkNode work;
    std::vector<ayni_audit_record_v1> captured;
    uint32_t executor_stops = 0;
    uint64_t next_nonce = 10;
    uint64_t command_id = 100;
    uint64_t now_us = 1000000;
    bool hold_motor = false;

    struct Active {
        ayni_attempt_ref_v1 attempt{};
        ayni_execution_permit_v1 permit{};
        ayni_provider_completion_t *completion = nullptr;
        bool stopped = false;
        bool posted = false;
    };
    std::optional<Active> active;
    uint64_t last_permit_id = 0;
    uint64_t last_operation_id = 0;
    bool last_stopped = false;

    void drain_audit() {
        std::array<ayni_audit_record_v1, 16> batch{};
        for (;;) {
            uint32_t count = 0;
            ASSERT_EQ(ayni_take_audit_records(work.kernel(), batch.data(),
                                              static_cast<uint32_t>(batch.size()), &count),
                      AYNI_OK);
            if (count == 0U) {
                break;
            }
            for (uint32_t i = 0; i < count; ++i) {
                captured.push_back(batch[i]);
            }
        }
    }

    const ayni_audit_record_v1 *find(ayni_audit_kind_t kind, uint32_t required_flags = 0U) const {
        for (const auto &rec : captured) {
            if (rec.kind == kind && (rec.flags & required_flags) == required_flags) {
                return &rec;
            }
        }
        return nullptr;
    }

    static ayni_status_t on_challenge(void *ctx, ayni_attempt_ref_v1 req,
                                      const ayni_permit_epochs_v1 *,
                                      ayni_executor_completion_t *completion) {
        auto &self = *static_cast<AuditFixture *>(ctx);
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_CHALLENGE;
        resp.request = req;
        resp.status = AYNI_OK;
        resp.output.nonce = {control_domain, self.next_nonce++};
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_renew(void *, ayni_attempt_ref_v1 req, const ayni_permit_renewal_v1 *,
                                  ayni_executor_completion_t *completion) {
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_RENEW;
        resp.request = req;
        resp.status = AYNI_OK;
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_stop(void *ctx, ayni_attempt_ref_v1 req, uint64_t permit_id,
                                 ayni_executor_completion_t *completion) {
        auto &self = *static_cast<AuditFixture *>(ctx);
        ++self.executor_stops;
        if (self.active && self.active->permit.permit_id == permit_id) {
            self.active->stopped = true;
        }
        if (self.last_permit_id == permit_id) {
            self.last_stopped = true;
        }
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_STOP;
        resp.request = req;
        resp.status = AYNI_OK;
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_status(void *ctx, ayni_attempt_ref_v1 req, uint64_t permit_id,
                                   ayni_executor_completion_t *completion) {
        auto &self = *static_cast<AuditFixture *>(ctx);
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_STATUS;
        resp.request = req;
        const bool match_active = self.active && self.active->permit.permit_id == permit_id;
        const bool match_last = self.last_permit_id == permit_id;
        if (!match_active && !match_last) {
            resp.status = AYNI_NOT_FOUND;
            return ayni_executor_post(completion, &resp);
        }
        const bool is_stopped = match_active ? self.active->stopped : self.last_stopped;
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

    static ayni_submit_result_t on_submit(void *ctx, const ayni_provider_request_v1 *req,
                                          ayni_provider_completion_t *completion) {
        auto &self = *static_cast<AuditFixture *>(ctx);
        const bool is_motor = req->execution_permit != nullptr;
        if (!is_motor || !self.hold_motor) {
            ayni_provider_result_v1 res{};
            res.struct_size = sizeof(res);
            res.attempt = req->attempt;
            res.outcome = AYNI_CALL_COMPLETED;
            res.status = AYNI_OK;
            res.effect = AYNI_CALL_EFFECT_COMPLETE;
            res.result = text("ok");
            ayni_provider_post(completion, &res);
            return AYNI_SUBMIT_ACCEPTED;
        }
        self.active = Active{req->attempt, *req->execution_permit, completion, false, false};
        self.last_permit_id = req->execution_permit->permit_id;
        self.last_operation_id = req->execution_permit->operation_id;
        self.last_stopped = false;
        return AYNI_SUBMIT_ACCEPTED;
    }

    static ayni_submit_result_t on_cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<AuditFixture *>(ctx);
        if (self.active && !self.active->posted &&
            self.active->attempt.request_id == attempt.request_id) {
            self.active->posted = true;
            self.active->stopped = true;
            self.last_stopped = true;
            ayni_provider_result_v1 res{};
            res.struct_size = sizeof(res);
            res.attempt = attempt;
            res.outcome = AYNI_CALL_CANCELLED;
            res.status = AYNI_OK;
            res.reason = AYNI_REASON_NONE;
            res.effect = AYNI_CALL_EFFECT_PARTIAL;
            res.result = text("stopped");
            ayni_executor_status_v1 inline_status{};
            inline_status.struct_size = sizeof(inline_status);
            inline_status.target = AYNI_TARGET_UNVERIFIED;
            inline_status.control.struct_size = sizeof(inline_status.control);
            inline_status.control.permit_id = self.active->permit.permit_id;
            inline_status.control.operation_id = self.active->permit.operation_id;
            inline_status.control.phase = AYNI_CONTROL_STOPPED;
            inline_status.control.stop_cause = AYNI_CONTROL_STOP_REQUESTED;
            res.execution_status = &inline_status;
            ayni_provider_post(self.active->completion, &res);
        }
        return AYNI_SUBMIT_ACCEPTED;
    }

    void setup(uint32_t audit_capacity) {
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
        cfg.reflex_capacity = 4;
        cfg.reflex_event_capacity = 8;
        cfg.audit_capacity = audit_capacity;

        work.additional_registration = [this](ayni_kernel_t *k) {
            const ayni_device_desc_v1 dev{sizeof(dev), 0, text("bot.dev")};
            ASSERT_EQ(ayni_register_device(k, &dev), AYNI_OK);
            const ayni_component_desc_v1 comp{sizeof(comp), 0, text("bot.motor"), text("bot.dev")};
            ASSERT_EQ(ayni_register_component(k, &comp), AYNI_OK);
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
            ASSERT_EQ(ayni_register_resource(k, &res), AYNI_OK);
            ASSERT_EQ(ayni_set_resource_health(k, text("motor.actuator"), AYNI_RESOURCE_ONLINE),
                      AYNI_OK);

            // Physical EPHEMERAL capability motor.run v3.
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
            const ayni_capability_desc_v1 motor_cap{sizeof(motor_cap), 3, text("motor.run"), 3, 0,
                                                    fields.data()};
            ASSERT_EQ(ayni_register_capability(k, &motor_cap), AYNI_OK);

            // Durable software capability software.log v1.
            const ayni_capability_desc_v1 dur_cap{sizeof(dur_cap), 1, text("software.log"), 0, 0,
                                                  nullptr};
            ASSERT_EQ(ayni_register_capability(k, &dur_cap), AYNI_OK);

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
            const ayni_business_contract_v1 motor_biz{
                {schema_nodes.data(), static_cast<uint32_t>(schema_nodes.size())},
                {&result_schema, 1},
                3,
                0,
                projections.data()};
            const ayni_call_resource_v1 need{text("motor.actuator"), 1, 0};
            const ayni_call_capability_v1 motor_cc{
                sizeof(motor_cc),
                3,
                text("motor.run"),
                {sizeof(ayni_call_contract_v1), AYNI_CALL_ACTION, AYNI_CALL_TOOL,
                 AYNI_CALL_EFFECT_PHYSICAL, AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE,
                 AYNI_CALL_CANCEL_REQUEST, 0, 65000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT,
                 768, 128, 1, &need, nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                &motor_biz};
            ASSERT_EQ(ayni_register_call_capability(k, &motor_cc), AYNI_OK);

            ayni_schema_node_v1 dur_in{};
            dur_in.kind = AYNI_VALUE_OBJECT;
            dur_in.subtree_end = 1;
            const ayni_business_contract_v1 dur_biz{
                {&dur_in, 1}, {&result_schema, 1}, 0, 0, nullptr};
            const ayni_call_capability_v1 dur_cc{
                sizeof(dur_cc),
                1,
                text("software.log"),
                {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0u,
                 AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_NONE, 0, 5000,
                 AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 64, 128, 0, nullptr, nullptr,
                 AYNI_CALL_DURABILITY_DURABLE, 0},
                &dur_biz};
            ASSERT_EQ(ayni_register_call_capability(k, &dur_cc), AYNI_OK);

            const std::array<ayni_call_capability_v1, 2> offers{motor_cc, dur_cc};
            const ayni_provider_v1 prov{
                sizeof(prov),  1,         text("p.audit"), text("ep.audit"), {},     this, 2, 0,
                offers.data(), on_submit, on_cancel,       nullptr,          nullptr};
            ASSERT_EQ(ayni_register_provider(k, &prov), AYNI_OK);

            const ayni_binding_v1 b_motor{sizeof(b_motor),
                                          3,
                                          text("motor.run"),
                                          text("motor.run"),
                                          3,
                                          1,
                                          text("p.audit"),
                                          text("ep.audit"),
                                          {},
                                          0,
                                          0,
                                          nullptr};
            ASSERT_EQ(ayni_register_binding(k, &b_motor), AYNI_OK);
            const ayni_binding_v1 b_dur{sizeof(b_dur),
                                        1,
                                        text("software.log"),
                                        text("software.log"),
                                        1,
                                        1,
                                        text("p.audit"),
                                        text("ep.audit"),
                                        {},
                                        0,
                                        0,
                                        nullptr};
            ASSERT_EQ(ayni_register_binding(k, &b_dur), AYNI_OK);

            const ayni_observation_source_v1 obstacle{
                sizeof(obstacle),           1,
                text("sensor.obstacle_mm"), AYNI_OBSERVATION_I64,
                AYNI_OBSERVATION_MEASURED,  text("mm"),
                obstacle_validity_us};
            ASSERT_EQ(ayni_register_observation_source(k, &obstacle), AYNI_OK);

            std::array<ayni_scope_field_v1, 3> scene_fields{};
            const int64_t smins[3] = {-50, -50, 50};
            const int64_t smaxs[3] = {50, 50, 2000};
            for (uint32_t at = 0; at < 3; ++at) {
                scene_fields[at].struct_size = sizeof(scene_fields[at]);
                scene_fields[at].field_id = at + 1;
                scene_fields[at].allowed = fields[at].domain;
                scene_fields[at].allowed.values.integer = {smins[at], smaxs[at]};
            }
            const ayni_scene_rule_v1 srule{
                text("motor.run"), 3, 0, {sizeof(ayni_scope_v1), 3, scene_fields.data()}};
            const ayni_scene_desc_v1 scene{sizeof(scene), 1,      text("motor.bench"), 1, 0,
                                           &srule,        nullptr};
            ASSERT_EQ(ayni_register_scene(k, &scene), AYNI_OK);

            const ayni_executor_v1 exec{sizeof(exec),
                                        1,
                                        text("motor.exec"),
                                        control_domain,
                                        1,
                                        1,
                                        1,
                                        1,
                                        lease_us,
                                        lease_us,
                                        lease_us,
                                        poll_us,
                                        this,
                                        on_challenge,
                                        on_renew,
                                        on_stop,
                                        on_status,
                                        AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF,
                                        0};
            ASSERT_EQ(ayni_register_executor(k, &exec), AYNI_OK);
            const std::array<ayni_execution_field_v1, 3> exec_fields{
                ayni_execution_field_v1{text("motor1_percent"), text("percent"), 1, 1},
                ayni_execution_field_v1{text("motor2_percent"), text("percent"), 2, 2},
                ayni_execution_field_v1{text("duration_ms"), text("ms"), 3, 3},
            };
            const ayni_execution_binding_v1 eb{sizeof(eb),
                                               3,
                                               text("motor.run"),
                                               text("motor.exec"),
                                               1,
                                               1,
                                               text("motor.bench"),
                                               AYNI_FORM_SESSION,
                                               lease_us,
                                               100000,
                                               3,
                                               exec_fields.data()};
            ASSERT_EQ(ayni_register_execution_binding(k, &eb), AYNI_OK);

            const ayni_observation_condition_v1 cond{text("sensor.obstacle_mm"), 1, 0, 0, 150,
                                                     obstacle_validity_us};
            const ayni_reflex_rule_v1 rule{sizeof(rule),
                                           1,
                                           text("reflex.stop"),
                                           AYNI_REFLEX_SOFT_STOP_RESOURCE,
                                           AYNI_REFLEX_TRIGGER_IN_RANGE,
                                           AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT,
                                           200,
                                           100000,
                                           text("motor.bench"),
                                           1,
                                           1,
                                           &cond,
                                           text("motor.actuator"),
                                           0,
                                           {},
                                           0,
                                           0,
                                           {},
                                           0,
                                           0,
                                           nullptr};
            ASSERT_EQ(ayni_register_reflex_rule(k, &rule), AYNI_OK);

            auto motor_gt = goal_type_desc("motor.goal", 1);
            motor_gt.validate = [](void *, ayni_bytes_v1) { return AYNI_OK; };
            motor_gt.export_requirements = [](void *, ayni_bytes_v1,
                                              const ayni_goal_requirement_sink_v1 *sink) {
                std::array<ayni_scope_field_v1, 3> rf{};
                const char *ru[3] = {"percent", "percent", "ms"};
                const int64_t rlo[3] = {-60, -60, 50};
                const int64_t rhi[3] = {60, 60, 5000};
                for (uint32_t at = 0; at < 3; ++at) {
                    rf[at].struct_size = sizeof(rf[at]);
                    rf[at].field_id = at + 1;
                    rf[at].allowed.struct_size = sizeof(rf[at].allowed);
                    rf[at].allowed.kind = AYNI_AUTHORITY_I64;
                    rf[at].allowed.unit = text(ru[at]);
                    rf[at].allowed.values.integer = {rlo[at], rhi[at]};
                }
                const ayni_goal_requirement_v1 need_m{
                    sizeof(need_m), 3, text("motor.run"), {sizeof(ayni_scope_v1), 3, rf.data()}};
                return sink->emit(sink->ctx, &need_m);
            };
            motor_gt.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT, 0};
            ASSERT_EQ(ayni_register_goal_type(k, &motor_gt), AYNI_OK);

            auto dur_gt = goal_type_desc("dur.goal", 1);
            dur_gt.validate = [](void *, ayni_bytes_v1) { return AYNI_OK; };
            dur_gt.export_requirements = [](void *, ayni_bytes_v1,
                                            const ayni_goal_requirement_sink_v1 *sink) {
                const ayni_goal_requirement_v1 need_d{
                    sizeof(need_d), 1, text("software.log"), {sizeof(ayni_scope_v1), 0, nullptr}};
                return sink->emit(sink->ctx, &need_d);
            };
            ASSERT_EQ(ayni_register_goal_type(k, &dur_gt), AYNI_OK);
        };
        work.run();
        if (work.node().sim().now_us() < 1000000U) {
            work.node().sim().advance_us(1000000U - work.node().sim().now_us());
        }
        now_us = work.node().sim().now_us();
    }

    void step(uint64_t advance_us = step_tick_us) {
        now_us += advance_us;
        work.node().sim().advance_us(advance_us);
        work.node().sim().complete_storage();
        work.node().step();
    }

    void flush_durable() {
        step(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
        step(step_tick_us);
    }

    void grant_all() {
        std::array<ayni_scope_field_v1, 3> fields{};
        const char *units[3] = {"percent", "percent", "ms"};
        const int64_t mins[3] = {-60, -60, 50};
        const int64_t maxs[3] = {60, 60, 5000};
        for (uint32_t at = 0; at < 3; ++at) {
            fields[at].struct_size = sizeof(fields[at]);
            fields[at].field_id = at + 1;
            fields[at].allowed.struct_size = sizeof(fields[at].allowed);
            fields[at].allowed.kind = AYNI_AUTHORITY_I64;
            fields[at].allowed.unit = text(units[at]);
            fields[at].allowed.values.integer = {mins[at], maxs[at]};
        }
        ayni_grant_create_v1 gm{};
        gm.struct_size = sizeof(gm);
        gm.capability_version = 3;
        gm.capability_id = text("motor.run");
        gm.subject_instance_id = work.instance(agent_a);
        gm.lifetime = AYNI_GRANT_PERMANENT;
        gm.scope = {sizeof(ayni_scope_v1), 3, fields.data()};
        auto in = work.intent(AYNI_INTENT_GRANT_CREATE, &gm);
        in.command_id = ++command_id;
        ASSERT_EQ(work.submit(in).status, AYNI_OK);
        flush_durable();

        ayni_grant_create_v1 gd{};
        gd.struct_size = sizeof(gd);
        gd.capability_version = 1;
        gd.capability_id = text("software.log");
        gd.subject_instance_id = work.instance(agent_a);
        gd.lifetime = AYNI_GRANT_PERMANENT;
        gd.scope = {sizeof(ayni_scope_v1), 0, nullptr};
        in = work.intent(AYNI_INTENT_GRANT_CREATE, &gd);
        in.command_id = ++command_id;
        ASSERT_EQ(work.submit(in).status, AYNI_OK);
        flush_durable();
    }

    void set_scene(bool enable) {
        ayni_scene_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_scene(work.kernel(), text("motor.bench"), &info), AYNI_OK);
        const ayni_scene_set_enabled_v1 cmd{sizeof(cmd), enable ? 1U : 0U, text("motor.bench"),
                                            enable ? 1U : 0U, 0};
        auto in = work.intent(AYNI_INTENT_SCENE_SET_ENABLED, &cmd, info.revision);
        in.command_id = ++command_id;
        ASSERT_EQ(work.submit(in).status, AYNI_OK);
        flush_durable();
    }

    void start_motor(const char *step_key) {
        auto seg = make_segment(30, 30, 500);
        work.strategy().answer = [seg, step_key](const ayni_task_snapshot_v1 &snap,
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
            req.value = {seg.data(), static_cast<uint32_t>(seg.size())};
            req.mode = AYNI_CALL_MODE_FOREGROUND;
            return out.request_call(out.call_context, &req);
        };
        const auto body = WorkNode::create_body(work.instance(agent_a), 0, "motor.goal", 1, {},
                                                AYNI_TRIGGER_ONCE);
        ASSERT_EQ(work.create(body).status, AYNI_OK);
    }

    void start_durable(const char *step_key) {
        work.strategy().answer = [step_key](const ayni_task_snapshot_v1 &snap,
                                            ayni_policy_output_v1 &out) {
            ayni_task_call_result_v1 prior{};
            prior.struct_size = sizeof(prior);
            if (snap.call_result(snap.call_context, text(step_key), &prior) == AYNI_OK) {
                return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
            }
            ayni_value_node_v1 obj{};
            obj.kind = AYNI_VALUE_OBJECT;
            obj.subtree_end = 1;
            ayni_call_request_v1 req{};
            req.struct_size = sizeof(req);
            req.kind = AYNI_POLICY_REQUEST_CALL;
            req.binding_version = 1;
            req.step_key = text(step_key);
            req.binding_id = text("software.log");
            req.value = {&obj, 1};
            req.mode = AYNI_CALL_MODE_FOREGROUND;
            return out.request_call(out.call_context, &req);
        };
        const auto body =
            WorkNode::create_body(work.instance(agent_a), 0, "dur.goal", 1, {}, AYNI_TRIGGER_ONCE);
        ASSERT_EQ(work.create(body).status, AYNI_OK);
    }
};

// ADR-0113: captures all 7 audit categories across lifecycle, ephemeral/durable calls, Scene
// denial, physical actuator quarantine/release, preemption, and deterministic ReflexRule firing.
TEST(AuditRing, captures_all_seven_categories_and_reentrancy_guard) {
    AuditFixture fx;
    fx.setup(64);
    fx.drain_audit();

    bool saw_recovering = false, saw_running = false;
    for (const auto &rec : fx.captured) {
        if (rec.kind == AYNI_AUDIT_LIFECYCLE && rec.detail_code == AYNI_LIFECYCLE_RECOVERING) {
            saw_recovering = true;
        }
        if (rec.kind == AYNI_AUDIT_LIFECYCLE && rec.detail_code == AYNI_LIFECYCLE_RUNNING) {
            saw_running = true;
        }
    }
    EXPECT_TRUE(saw_recovering);
    EXPECT_TRUE(saw_running);
    fx.captured.clear();

    // Grant both capabilities, keep Scene disabled -> AUTHORITY_DENIED + callback reentrancy guard.
    fx.grant_all();
    ayni_status_t reentrant_status = AYNI_OK;
    fx.work.strategy().during = [&] {
        ayni_audit_info_v1 info{};
        info.struct_size = sizeof(info);
        reentrant_status = ayni_get_audit_info(fx.work.kernel(), &info);
    };
    fx.start_motor("m0");
    for (int i = 0; i < 6; ++i) {
        fx.step();
    }
    EXPECT_EQ(reentrant_status, AYNI_INVALID_STATE);
    fx.drain_audit();
    const auto *denied = fx.find(AYNI_AUDIT_AUTHORITY_DENIED, AYNI_AUDIT_FLAG_DENIED);
    ASSERT_NE(denied, nullptr);
    EXPECT_EQ(denied->status, AYNI_DENIED);
    EXPECT_EQ(denied->reason, AYNI_REASON_EXECUTION_SCENE);
    EXPECT_EQ(std::string_view(denied->label, denied->label_size), "motor.run");
    fx.captured.clear();

    // DURABLE Call terminal (no EPHEMERAL flag).
    fx.start_durable("d1");
    for (int i = 0; i < 12; ++i) {
        fx.step();
    }
    fx.drain_audit();
    const auto *dur_term = fx.find(AYNI_AUDIT_CALL_TERMINAL);
    ASSERT_NE(dur_term, nullptr);
    EXPECT_EQ(dur_term->flags & AYNI_AUDIT_FLAG_EPHEMERAL, 0U);
    EXPECT_EQ(dur_term->detail_code, AYNI_CALL_COMPLETED);
    EXPECT_EQ(std::string_view(dur_term->label, dur_term->label_size), "software.log");
    fx.captured.clear();

    // Enable Scene, hold an EPHEMERAL physical call, then fire ReflexRule SOFT_STOP_RESOURCE
    // -> REFLEX_EVENT, RESOURCE_PREEMPTED, ACTUATOR_QUARANTINE, ACTUATOR_RELEASE, CALL_TERMINAL.
    fx.set_scene(true);
    fx.hold_motor = true;
    fx.start_motor("m1");
    for (int i = 0; i < 12 && !fx.active.has_value(); ++i) {
        fx.step();
    }
    ASSERT_TRUE(fx.active.has_value());

    ayni_observation_post_v1 post{};
    post.struct_size = sizeof(post);
    post.boot_epoch = fx.work.node().info().boot_epoch;
    post.sample.struct_size = sizeof(post.sample);
    post.sample.source_version = 1;
    post.sample.source_id = text("sensor.obstacle_mm");
    post.sample.generation = 1;
    post.sample.sequence = 1;
    post.sample.captured_at_us = fx.now_us;
    post.sample.valid_for_us = obstacle_validity_us;
    post.sample.quality = AYNI_OBSERVATION_GOOD;
    post.sample.value = 50;
    ASSERT_EQ(ayni_publish_observation_latest(fx.work.kernel(), &post), AYNI_OK);
    for (int i = 0; i < 12; ++i) {
        fx.step();
    }
    fx.drain_audit();

    EXPECT_NE(fx.find(AYNI_AUDIT_REFLEX_EVENT, AYNI_AUDIT_FLAG_EPHEMERAL), nullptr);
    EXPECT_NE(fx.find(AYNI_AUDIT_RESOURCE_PREEMPTED, AYNI_AUDIT_FLAG_PREEMPTED), nullptr);
    EXPECT_NE(fx.find(AYNI_AUDIT_ACTUATOR_QUARANTINE, AYNI_AUDIT_FLAG_PHYSICAL), nullptr);
    EXPECT_NE(fx.find(AYNI_AUDIT_ACTUATOR_RELEASE, AYNI_AUDIT_FLAG_PHYSICAL), nullptr);
    const auto *eph_term =
        fx.find(AYNI_AUDIT_CALL_TERMINAL,
                AYNI_AUDIT_FLAG_EPHEMERAL | AYNI_AUDIT_FLAG_PHYSICAL | AYNI_AUDIT_FLAG_PREEMPTED);
    ASSERT_NE(eph_term, nullptr);
    EXPECT_EQ(eph_term->detail_code, AYNI_CALL_CANCELLED);
    fx.captured.clear();

    // Shutdown records DRAINING and STOPPED.
    ASSERT_EQ(ayni_begin_shutdown(fx.work.kernel()), AYNI_OK);
    for (int i = 0; i < 12 && fx.work.node().info().lifecycle != AYNI_LIFECYCLE_STOPPED; ++i) {
        fx.step();
    }
    fx.drain_audit();
    bool saw_draining = false, saw_stopped = false;
    for (const auto &rec : fx.captured) {
        if (rec.kind == AYNI_AUDIT_LIFECYCLE && rec.detail_code == AYNI_LIFECYCLE_DRAINING) {
            saw_draining = true;
        }
        if (rec.kind == AYNI_AUDIT_LIFECYCLE && rec.detail_code == AYNI_LIFECYCLE_STOPPED) {
            saw_stopped = true;
        }
    }
    EXPECT_TRUE(saw_draining);
    EXPECT_TRUE(saw_stopped);
}

// ADR-0113: full ring overwrites oldest unread slots, increments dropped_overflow, exposes seq
// gaps, never touches WAL on drain, and enforces argument/struct_size validation.
TEST(AuditRing, overflow_overwrites_oldest_and_disabled_ring_has_zero_overhead) {
    AuditFixture fx;
    fx.setup(2); // Tiny 2-slot ring forces wrap-around across startup + two durable calls.
    fx.grant_all();
    fx.start_durable("d1");
    for (int i = 0; i < 10; ++i) {
        fx.step();
    }
    fx.start_durable("d2");
    for (int i = 0; i < 10; ++i) {
        fx.step();
    }

    ayni_audit_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_audit_info(fx.work.kernel(), &info), AYNI_OK);
    EXPECT_EQ(info.capacity, 2U);
    EXPECT_EQ(info.pending, 2U);
    EXPECT_GT(info.dropped_overflow, 0U);
    EXPECT_GT(info.next_seq, 3U);

    ayni_audit_info_v1 bad_small{};
    bad_small.struct_size = sizeof(bad_small) - 4U;
    EXPECT_EQ(ayni_get_audit_info(fx.work.kernel(), &bad_small), AYNI_INVALID_ARGUMENT);
    ayni_audit_info_v1 bad_large{};
    bad_large.struct_size = sizeof(bad_large) + 4U;
    EXPECT_EQ(ayni_get_audit_info(fx.work.kernel(), &bad_large), AYNI_UNSUPPORTED_VERSION);
    uint32_t out_count = 99;
    EXPECT_EQ(ayni_take_audit_records(fx.work.kernel(), nullptr, 1, &out_count),
              AYNI_INVALID_ARGUMENT);

    const std::size_t log_before = fx.work.node().sim().log().size();
    std::array<ayni_audit_record_v1, 4> recs{};
    ASSERT_EQ(ayni_take_audit_records(fx.work.kernel(), recs.data(), 4, &out_count), AYNI_OK);
    EXPECT_EQ(out_count, 2U);
    EXPECT_EQ(fx.work.node().sim().log().size(), log_before);
    EXPECT_GT(recs[0].seq, 1U);
    EXPECT_EQ(recs[1].seq, recs[0].seq + 1U);
    EXPECT_EQ(recs[0].struct_size, sizeof(ayni_audit_record_v1));

    ASSERT_EQ(ayni_get_audit_info(fx.work.kernel(), &info), AYNI_OK);
    EXPECT_EQ(info.pending, 0U);
    EXPECT_EQ(info.drained_total, 2U);

    AuditFixture disabled;
    disabled.setup(0);
    ASSERT_EQ(ayni_get_audit_info(disabled.work.kernel(), &info), AYNI_OK);
    EXPECT_EQ(info.capacity, 0U);
    EXPECT_EQ(info.pending, 0U);
    EXPECT_EQ(info.dropped_overflow, 0U);
    ASSERT_EQ(ayni_take_audit_records(disabled.work.kernel(), recs.data(), 4, &out_count), AYNI_OK);
    EXPECT_EQ(out_count, 0U);
}

// ADR-0113: closing a live physical gate via Scene disable (latch_closed_gates) must still emit
// paired ACTUATOR_QUARANTINE and ACTUATOR_RELEASE audit records.
TEST(AuditRing, quarantine_and_release_pair_on_scene_disable) {
    AuditFixture fx;
    fx.setup(64);
    fx.grant_all();
    fx.set_scene(true);
    fx.hold_motor = true;
    fx.drain_audit();
    fx.captured.clear();

    fx.start_motor("m_scene_close");
    for (int i = 0; i < 12 && !fx.active.has_value(); ++i) {
        fx.step();
    }
    ASSERT_TRUE(fx.active.has_value());

    // Disabling Scene closes the live gate via latch_closed_gates().
    fx.set_scene(false);
    for (int i = 0; i < 12; ++i) {
        fx.step();
    }
    fx.drain_audit();

    EXPECT_NE(fx.find(AYNI_AUDIT_ACTUATOR_QUARANTINE, AYNI_AUDIT_FLAG_PHYSICAL), nullptr);
    EXPECT_NE(fx.find(AYNI_AUDIT_ACTUATOR_RELEASE, AYNI_AUDIT_FLAG_PHYSICAL), nullptr);
}

} // namespace
