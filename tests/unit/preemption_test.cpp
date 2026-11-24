// ADR-0111: Phase 2 resource-level EPHEMERAL preemption (ABI 0.58), AYNI_REASON_PREEMPTED,
// fail-closed stop-proof quarantine, all-or-none multi-resource preemption, authorization/Scene/
// Schema preflight invariance, ToolLoop preemption validation, and zero-Flash background
// ConversationSession volatile draft discard.

#include "work_node.h"
#include "workspace_fixture.h"

#include <ayni/calls.h>
#include <ayni/context.h>
#include <ayni/execution.h>
#include <ayni/observations.h>
#include <ayni/runtime/conversation.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <ayni/scenes.h>
#include <ayni/strategies/builtin.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ayni::test;

constexpr uint64_t step_tick_us = 10000;
constexpr uint32_t lease_us = 400000;
constexpr uint32_t poll_period_us = 200000;
constexpr uint64_t control_domain_id = 77;
constexpr uint32_t execution_field_id = 1;

// Colocated physical executor + provider stub supporting controllable stop responses,
// inline stop proofs, and stop-failure/quarantine simulation.
struct PreemptTestExecutor {
    enum class StopBehavior {
        inline_stopped_on_cancel, // Posts CANCELLED + inline STOPPED immediately when cancelled.
        fail_stop_stay_active,    // Fails stop and reports ACTIVE until allowed_to_stop = true.
    };

    StopBehavior stop_behavior = StopBehavior::inline_stopped_on_cancel;
    bool allowed_to_stop = true;
    uint32_t stop_calls = 0;
    uint32_t status_calls = 0;
    uint32_t submit_calls = 0;
    uint32_t cancel_calls = 0;
    uint64_t next_nonce = 10;

    struct Active {
        ayni_attempt_ref_v1 attempt{};
        uint64_t call_id = 0;
        uint64_t permit_id = 0;
        uint64_t operation_id = 0;
        ayni_provider_completion_t *completion = nullptr;
        bool stopped = false;
        bool posted = false;
    };
    std::optional<Active> active;
    uint64_t last_permit_id = 0;
    uint64_t last_operation_id = 0;
    bool last_stopped = false;
    std::vector<uint64_t> submitted_calls;

    static ayni_status_t on_challenge(void *ctx, ayni_attempt_ref_v1 request,
                                      const ayni_permit_epochs_v1 *,
                                      ayni_executor_completion_t *completion) {
        auto &self = *static_cast<PreemptTestExecutor *>(ctx);
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_CHALLENGE;
        resp.request = request;
        resp.status = AYNI_OK;
        resp.output.nonce = {control_domain_id, self.next_nonce++};
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_renew(void *, ayni_attempt_ref_v1 request,
                                  const ayni_permit_renewal_v1 *,
                                  ayni_executor_completion_t *completion) {
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_RENEW;
        resp.request = request;
        resp.status = AYNI_OK;
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_stop(void *ctx, ayni_attempt_ref_v1 request, uint64_t permit_id,
                                 ayni_executor_completion_t *completion) {
        auto &self = *static_cast<PreemptTestExecutor *>(ctx);
        ++self.stop_calls;
        const bool can_stop =
            self.stop_behavior == StopBehavior::inline_stopped_on_cancel || self.allowed_to_stop;
        if (can_stop) {
            if (self.active && self.active->permit_id == permit_id) {
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
        resp.status = can_stop ? AYNI_OK : AYNI_INTERNAL;
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_status(void *ctx, ayni_attempt_ref_v1 request, uint64_t permit_id,
                                   ayni_executor_completion_t *completion) {
        auto &self = *static_cast<PreemptTestExecutor *>(ctx);
        ++self.status_calls;
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_STATUS;
        resp.request = request;
        const bool match_active = self.active && self.active->permit_id == permit_id;
        const bool match_last = self.last_permit_id == permit_id;
        if (!match_active && !match_last) {
            resp.status = AYNI_NOT_FOUND;
            return ayni_executor_post(completion, &resp);
        }
        const bool is_stopped =
            self.allowed_to_stop && (self.stop_behavior == StopBehavior::fail_stop_stay_active ||
                                     (match_active ? self.active->stopped : self.last_stopped));
        const uint64_t op_id = match_active ? self.active->operation_id : self.last_operation_id;
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
        auto &self = *static_cast<PreemptTestExecutor *>(ctx);
        ++self.submit_calls;
        if (!request || !request->execution_permit || !completion) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.active = Active{request->attempt,
                             request->logical_call_id,
                             request->execution_permit->permit_id,
                             request->execution_permit->operation_id,
                             completion,
                             false,
                             false};
        self.last_permit_id = request->execution_permit->permit_id;
        self.last_operation_id = request->execution_permit->operation_id;
        self.last_stopped = false;
        self.submitted_calls.push_back(request->logical_call_id);
        return AYNI_SUBMIT_ACCEPTED;
    }

    static ayni_submit_result_t on_cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<PreemptTestExecutor *>(ctx);
        ++self.cancel_calls;
        if (!self.active || self.active->posted ||
            self.active->attempt.request_id != attempt.request_id) {
            return AYNI_SUBMIT_ACCEPTED;
        }
        if (self.stop_behavior == StopBehavior::inline_stopped_on_cancel) {
            self.finish_active(AYNI_CALL_CANCELLED, AYNI_OK, AYNI_CALL_EFFECT_PARTIAL, "stopped",
                               true);
        } else {
            // Omit stop proof and simulate failed hardware stop -> quarantine.
            self.finish_active(AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_CALL_EFFECT_UNKNOWN,
                               nullptr, false);
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
            inline_status.control.permit_id = active->permit_id;
            inline_status.control.operation_id = active->operation_id;
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
                control_domain_id,
                1,
                1,
                1,
                1,
                lease_us,
                lease_us,
                lease_us,
                poll_period_us,
                this,
                on_challenge,
                on_renew,
                on_stop,
                on_status,
                AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF,
                0};
    }
};

// Simple software provider that tracks multiple concurrent in-flight attempts by request_id.
struct MultiStubProvider {
    struct Slot {
        ayni_attempt_ref_v1 attempt{};
        uint64_t call_id = 0;
        ayni_provider_completion_t *completion = nullptr;
    };
    bool auto_cancel = true;
    uint32_t submit_calls = 0;
    uint32_t cancel_calls = 0;
    std::vector<Slot> in_flight;

    static ayni_submit_result_t on_submit(void *ctx, const ayni_provider_request_v1 *request,
                                          ayni_provider_completion_t *completion) {
        auto &self = *static_cast<MultiStubProvider *>(ctx);
        ++self.submit_calls;
        self.in_flight.push_back({request->attempt, request->logical_call_id, completion});
        return AYNI_SUBMIT_ACCEPTED;
    }

    static ayni_submit_result_t on_cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
        auto &self = *static_cast<MultiStubProvider *>(ctx);
        ++self.cancel_calls;
        if (!self.auto_cancel) {
            return AYNI_SUBMIT_ACCEPTED;
        }
        for (auto it = self.in_flight.begin(); it != self.in_flight.end(); ++it) {
            if (it->attempt.request_id == attempt.request_id) {
                ayni_provider_result_v1 result{};
                result.struct_size = sizeof(result);
                result.outcome = AYNI_CALL_CANCELLED;
                result.attempt = it->attempt;
                result.status = AYNI_OK;
                result.effect = AYNI_CALL_EFFECT_NONE;
                result.result = text("cancelled");
                EXPECT_EQ(ayni_provider_post(it->completion, &result), AYNI_OK);
                self.in_flight.erase(it);
                break;
            }
        }
        return AYNI_SUBMIT_ACCEPTED;
    }

    bool complete_call(uint64_t call_id, const char *reply = "done") {
        for (auto it = in_flight.begin(); it != in_flight.end(); ++it) {
            if (it->call_id == call_id) {
                ayni_provider_result_v1 result{};
                result.struct_size = sizeof(result);
                result.outcome = AYNI_CALL_COMPLETED;
                result.attempt = it->attempt;
                result.status = AYNI_OK;
                result.effect = AYNI_CALL_EFFECT_COMPLETE;
                result.result = text(reply);
                EXPECT_EQ(ayni_provider_post(it->completion, &result), AYNI_OK);
                in_flight.erase(it);
                return true;
            }
        }
        return false;
    }
};

struct PreemptionFixture {
    WorkNode work;
    PreemptTestExecutor motor_exec;
    MultiStubProvider soft_provider;
    uint64_t next_command = 1;
    uint64_t motor_grant = 0;

    ayni_call_resource_v1 motor_res_req{text("motor.actuator"), 1, 0};
    ayni_call_resource_v1 work_res_req{text("work.slot"), 1, 0};
    ayni_call_resource_v1 aux_res_req{text("aux.actuator"), 1, 0};
    std::array<ayni_call_resource_v1, 2> both_res_req{
        {{text("work.slot"), 1, 0}, {text("aux.actuator"), 1, 0}}};

    std::array<ayni_schema_node_v1, 2> motor_input_schema{};
    ayni_schema_node_v1 text_schema{};
    ayni_authority_projection_v1 motor_projection{};
    ayni_business_contract_v1 motor_business{};
    ayni_business_contract_v1 text_business{};

    // Capabilities:
    //  motor_cap: motor.act (EPHEMERAL, physical, CANCEL_REQUEST, resource: motor.actuator)
    //  soft_caps[0]: motor.durable (DURABLE, ACTION, CANCEL_REQUEST, resource: work.slot)
    //  soft_caps[1]: motor.nocancel (EPHEMERAL, UNARY, CANCEL_NONE, resource: work.slot)
    //  soft_caps[2]: aux.durable (DURABLE, UNARY, CANCEL_REQUEST, resource: aux.actuator)
    //  soft_caps[3]: dual.ephem (EPHEMERAL, UNARY, CANCEL_REQUEST, resources: {work.slot,
    //  aux.actuator}) soft_caps[4]: work.ephem (EPHEMERAL, UNARY, CANCEL_REQUEST, resource:
    //  work.slot)
    ayni_call_capability_v1 motor_cap{};
    std::array<ayni_call_capability_v1, 5> soft_caps{};

    void configure_node() {
        auto &cfg = work.node().config();
        cfg.call_capacity = 12;
        cfg.call_input_max_bytes = 256;
        cfg.call_result_max_bytes = 128;
        cfg.call_capability_capacity = 8;
        cfg.provider_capacity = 4;
        cfg.binding_capacity = 10;
        cfg.resource_capacity = 6;
        cfg.resource_request_capacity = 12;
        cfg.goal_type_capacity = 6;
        cfg.goal_capacity = 8;
        cfg.goal_requirement_capacity = 8;
        cfg.task_capacity = 8;
        cfg.task_step_capacity = 12;
        cfg.task_checkpoint_max_bytes = 64;
        cfg.scene_capacity = 4;
        cfg.observation_source_capacity = 2;
        cfg.executor_capacity = 2;
        cfg.execution_binding_capacity = 4;
        cfg.command_capacity = 24;
        cfg.grant_capacity = 16;
        cfg.storage_buffer_bytes = 0;
        work.node().on_create([this](ayni_kernel_t *kernel) { register_all(kernel); });
    }

    void register_all(ayni_kernel_t *kernel) {
        const auto owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        const auto agent = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);
        const auto strat =
            strategy_desc(strategy_id, 1, ScriptedStrategy::decide, &work.strategy(), 2, 64);
        ASSERT_EQ(ayni_register_strategy(kernel, &strat), AYNI_OK);

        // Capability authority descriptors.
        ayni_authority_field_desc_v1 speed_field{};
        speed_field.struct_size = sizeof(speed_field);
        speed_field.field_id = execution_field_id;
        speed_field.usage = AYNI_AUTHORITY_PARAMETER;
        speed_field.domain.struct_size = sizeof(speed_field.domain);
        speed_field.domain.kind = AYNI_AUTHORITY_I64;
        speed_field.domain.unit = text("pct");
        speed_field.domain.values.integer = {-100, 100};

        const ayni_capability_desc_v1 motor_auth{
            sizeof(ayni_capability_desc_v1), 1, text("motor.act"), 1, 0, &speed_field};
        ASSERT_EQ(ayni_register_capability(kernel, &motor_auth), AYNI_OK);

        constexpr const char *soft_names[] = {"motor.durable", "motor.nocancel", "aux.durable",
                                              "dual.ephem", "work.ephem"};
        for (const char *name : soft_names) {
            const ayni_capability_desc_v1 desc{sizeof(desc), 1, text(name), 0, 0, nullptr};
            ASSERT_EQ(ayni_register_capability(kernel, &desc), AYNI_OK);
        }

        // Business schemas.
        motor_input_schema = {};
        motor_input_schema[0].kind = AYNI_VALUE_OBJECT;
        motor_input_schema[0].subtree_end = 2;
        motor_input_schema[1].kind = AYNI_VALUE_I64;
        motor_input_schema[1].field_id = execution_field_id;
        motor_input_schema[1].subtree_end = 2;
        motor_input_schema[1].flags = AYNI_SCHEMA_REQUIRED;
        motor_input_schema[1].name = text("speed");
        motor_input_schema[1].unit = text("pct");
        motor_input_schema[1].min_i64 = -100;
        motor_input_schema[1].max_i64 = 100;
        motor_projection = {};
        motor_projection.field_id = execution_field_id;
        motor_projection.path_count = 1;
        motor_projection.path[0] = execution_field_id;
        motor_projection.unit = text("pct");

        text_schema = {};
        text_schema.kind = AYNI_VALUE_TEXT;
        text_schema.subtree_end = 1;
        text_schema.max_count = 64;

        motor_business = {
            {motor_input_schema.data(), 2}, {&text_schema, 1}, 1, 0, &motor_projection};
        text_business = {{&text_schema, 1}, {&text_schema, 1}, 0, 0, nullptr};

        motor_cap = {sizeof(motor_cap),
                     1,
                     text("motor.act"),
                     {sizeof(ayni_call_contract_v1), AYNI_CALL_ACTION, AYNI_CALL_TOOL,
                      AYNI_CALL_EFFECT_PHYSICAL, AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE,
                      AYNI_CALL_CANCEL_REQUEST, 0, 30000, AYNI_CALL_INPUT_VALUE,
                      AYNI_CALL_RESULT_TEXT, 128, 64, 1, &motor_res_req, nullptr,
                      AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                     &motor_business};
        ASSERT_EQ(ayni_register_call_capability(kernel, &motor_cap), AYNI_OK);

        // Soft capabilities sharing work.slot and aux.actuator.
        soft_caps[0] = {sizeof(ayni_call_capability_v1),
                        1,
                        text("motor.durable"),
                        {sizeof(ayni_call_contract_v1), AYNI_CALL_ACTION, AYNI_CALL_TOOL, 0,
                         AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST,
                         0, 30000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 64, 1,
                         &work_res_req, nullptr, AYNI_CALL_DURABILITY_DURABLE, 0},
                        &text_business};
        soft_caps[1] = {sizeof(ayni_call_capability_v1),
                        1,
                        text("motor.nocancel"),
                        {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                         AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_NONE, 0,
                         30000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 64, 1,
                         &work_res_req, nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                        &text_business};
        soft_caps[2] = {sizeof(ayni_call_capability_v1),
                        1,
                        text("aux.durable"),
                        {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                         AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST,
                         0, 30000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 64, 1,
                         &aux_res_req, nullptr, AYNI_CALL_DURABILITY_DURABLE, 0},
                        &text_business};
        soft_caps[3] = {sizeof(ayni_call_capability_v1),
                        1,
                        text("dual.ephem"),
                        {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                         AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                         30000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 64, 2,
                         both_res_req.data(), nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                        &text_business};
        soft_caps[4] = {sizeof(ayni_call_capability_v1),
                        1,
                        text("work.ephem"),
                        {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                         AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                         30000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 64, 1,
                         &work_res_req, nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                        &text_business};
        for (auto &cap : soft_caps) {
            ASSERT_EQ(ayni_register_call_capability(kernel, &cap), AYNI_OK);
        }

        // Resources: motor.actuator (ACTUATOR), work.slot (exclusive), aux.actuator (exclusive).
        const ayni_resource_desc_v1 motor_res{sizeof(motor_res),
                                              AYNI_RESOURCE_EXCLUSIVE,
                                              text("motor.actuator"),
                                              1,
                                              AYNI_RESOURCE_ACTUATOR,
                                              0,
                                              0,
                                              nullptr,
                                              {}};
        const ayni_resource_desc_v1 work_res{
            sizeof(work_res), AYNI_RESOURCE_EXCLUSIVE, text("work.slot"), 1, 0, 0, 0, nullptr, {}};
        const ayni_resource_desc_v1 aux_res{sizeof(aux_res),
                                            AYNI_RESOURCE_EXCLUSIVE,
                                            text("aux.actuator"),
                                            1,
                                            0,
                                            0,
                                            0,
                                            nullptr,
                                            {}};
        ASSERT_EQ(ayni_register_resource(kernel, &motor_res), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, motor_res.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        ASSERT_EQ(ayni_register_resource(kernel, &work_res), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, work_res.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        ASSERT_EQ(ayni_register_resource(kernel, &aux_res), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, aux_res.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);

        // Providers & bindings.
        const ayni_provider_v1 mp{sizeof(mp),
                                  1,
                                  text("motor.prov"),
                                  text("motor.ep"),
                                  {},
                                  &motor_exec,
                                  1,
                                  0,
                                  &motor_cap,
                                  PreemptTestExecutor::on_submit,
                                  PreemptTestExecutor::on_cancel,
                                  nullptr,
                                  nullptr};
        const ayni_provider_v1 sp{sizeof(sp),
                                  1,
                                  text("soft.prov"),
                                  text("soft.ep"),
                                  {},
                                  &soft_provider,
                                  static_cast<uint32_t>(soft_caps.size()),
                                  0,
                                  soft_caps.data(),
                                  MultiStubProvider::on_submit,
                                  MultiStubProvider::on_cancel,
                                  nullptr,
                                  nullptr};
        ASSERT_EQ(ayni_register_provider(kernel, &mp), AYNI_OK);
        ASSERT_EQ(ayni_register_provider(kernel, &sp), AYNI_OK);

        const ayni_binding_v1 mb{sizeof(mb),
                                 1,
                                 text("motor.bind"),
                                 text("motor.act"),
                                 1,
                                 1,
                                 text("motor.prov"),
                                 text("motor.ep"),
                                 {},
                                 0,
                                 0,
                                 nullptr};
        const ayni_binding_v1 mb_alt{sizeof(mb_alt),
                                     1,
                                     text("motor.alt"),
                                     text("motor.act"),
                                     1,
                                     1,
                                     text("motor.prov"),
                                     text("motor.ep"),
                                     {},
                                     0,
                                     0,
                                     nullptr};
        ASSERT_EQ(ayni_register_binding(kernel, &mb), AYNI_OK);
        ASSERT_EQ(ayni_register_binding(kernel, &mb_alt), AYNI_OK);
        for (const char *name : soft_names) {
            const ayni_binding_v1 bind{
                sizeof(bind),    1,  text(name), text(name), 1,      1, text("soft.prov"),
                text("soft.ep"), {}, 0,          0,          nullptr};
            ASSERT_EQ(ayni_register_binding(kernel, &bind), AYNI_OK);
        }

        // Scenes (bench.scene enabled; alt.scene remains disabled), Executor, ExecutionBindings.
        ayni_scope_field_v1 scene_field{};
        scene_field.struct_size = sizeof(scene_field);
        scene_field.field_id = execution_field_id;
        scene_field.allowed.struct_size = sizeof(scene_field.allowed);
        scene_field.allowed.kind = AYNI_AUTHORITY_I64;
        scene_field.allowed.unit = text("pct");
        scene_field.allowed.values.integer = {-100, 100};
        const ayni_scene_rule_v1 scene_rule{
            text("motor.act"), 1, 0, {sizeof(ayni_scope_v1), 1, &scene_field}};
        const ayni_scene_desc_v1 scene_desc{sizeof(scene_desc), 1,      text("bench.scene"), 1, 0,
                                            &scene_rule,        nullptr};
        const ayni_scene_desc_v1 alt_scene_desc{
            sizeof(alt_scene_desc), 1, text("alt.scene"), 1, 0, &scene_rule, nullptr};
        ASSERT_EQ(ayni_register_scene(kernel, &scene_desc), AYNI_OK);
        ASSERT_EQ(ayni_register_scene(kernel, &alt_scene_desc), AYNI_OK);

        const auto exec_ports = motor_exec.ports();
        ASSERT_EQ(ayni_register_executor(kernel, &exec_ports), AYNI_OK);

        const ayni_execution_field_v1 exec_field{text("speed"), text("pct"), execution_field_id,
                                                 execution_field_id};
        const ayni_execution_binding_v1 exec_bind{sizeof(exec_bind),
                                                  1,
                                                  text("motor.bind"),
                                                  exec_ports.executor_id,
                                                  1,
                                                  1,
                                                  text("bench.scene"),
                                                  AYNI_FORM_PROCEDURE,
                                                  lease_us,
                                                  lease_us,
                                                  1,
                                                  &exec_field};
        const ayni_execution_binding_v1 alt_exec_bind{sizeof(alt_exec_bind),
                                                      1,
                                                      text("motor.alt"),
                                                      exec_ports.executor_id,
                                                      1,
                                                      1,
                                                      text("alt.scene"),
                                                      AYNI_FORM_PROCEDURE,
                                                      lease_us,
                                                      lease_us,
                                                      1,
                                                      &exec_field};
        ASSERT_EQ(ayni_register_execution_binding(kernel, &exec_bind), AYNI_OK);
        ASSERT_EQ(ayni_register_execution_binding(kernel, &alt_exec_bind), AYNI_OK);

        // GoalTypes:
        //  fg_ephem: FOREGROUND + CANCEL_ON_REBOOT
        //  bg_ephem: BACKGROUND + CANCEL_ON_REBOOT (INTERVAL trigger, fires at delay=0)
        //  fg_durable: FOREGROUND + RESUME
        auto accept_any_param = [](void *, ayni_bytes_v1) { return AYNI_OK; };
        auto type = goal_type_desc("fg_ephem", 1);
        type.validate = accept_any_param;
        type.export_requirements = emit_all_requirements;
        type.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT, 0};
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);

        auto bg_type = goal_type_desc("bg_ephem", 1);
        bg_type.validate = accept_any_param;
        bg_type.export_requirements = emit_all_requirements;
        bg_type.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_INTERVAL);
        bg_type.occurrence_wait_max_us = AYNI_RESOURCE_WAIT_DEFAULT_US;
        bg_type.work_profile = {AYNI_WORK_BACKGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT,
                                AYNI_TRIGGER_PERIOD_MIN_US};
        ASSERT_EQ(ayni_register_goal_type(kernel, &bg_type), AYNI_OK);

        type.goal_type_id = text("fg_durable");
        type.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_RESUME, 0};
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
    }

    static ayni_status_t emit_all_requirements(void *, ayni_bytes_v1,
                                               const ayni_goal_requirement_sink_v1 *sink) {
        ayni_scope_field_v1 speed_field{};
        speed_field.struct_size = sizeof(speed_field);
        speed_field.field_id = execution_field_id;
        speed_field.allowed.struct_size = sizeof(speed_field.allowed);
        speed_field.allowed.kind = AYNI_AUTHORITY_I64;
        speed_field.allowed.unit = text("pct");
        speed_field.allowed.values.integer = {-100, 100};
        const ayni_goal_requirement_v1 m_req{
            sizeof(m_req), 1, text("motor.act"), {sizeof(ayni_scope_v1), 1, &speed_field}};
        auto rc = sink->emit(sink->ctx, &m_req);
        if (rc != AYNI_OK) {
            return rc;
        }
        constexpr const char *soft_names[] = {"motor.durable", "motor.nocancel", "aux.durable",
                                              "dual.ephem", "work.ephem"};
        for (const char *name : soft_names) {
            const ayni_goal_requirement_v1 req{
                sizeof(req), 1, text(name), {sizeof(ayni_scope_v1), 0, nullptr}};
            rc = sink->emit(sink->ctx, &req);
            if (rc != AYNI_OK) {
                return rc;
            }
        }
        return AYNI_OK;
    }

    void start() {
        configure_node();
        work.run();
        motor_grant = create_grant("motor.act", true);
        create_grant("motor.durable", false);
        create_grant("motor.nocancel", false);
        create_grant("aux.durable", false);
        create_grant("dual.ephem", false);
        create_grant("work.ephem", false);
        set_scene(true);
        pump(20);
    }

    uint64_t create_grant(const char *cap, bool with_speed) {
        ayni_scope_field_v1 speed_field{};
        speed_field.struct_size = sizeof(speed_field);
        speed_field.field_id = execution_field_id;
        speed_field.allowed.struct_size = sizeof(speed_field.allowed);
        speed_field.allowed.kind = AYNI_AUTHORITY_I64;
        speed_field.allowed.unit = text("pct");
        speed_field.allowed.values.integer = {-100, 100};
        const ayni_grant_create_v1 grant{
            sizeof(grant),
            1,
            text(cap),
            work.instance(agent_a),
            0,
            AYNI_GRANT_PERMANENT,
            0,
            0,
            {sizeof(ayni_scope_v1), with_speed ? 1U : 0U, with_speed ? &speed_field : nullptr}};
        auto intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
        intent.command_id = ++next_command;
        const auto accepted = work.submit(intent);
        EXPECT_EQ(accepted.status, AYNI_OK);
        return accepted.result.object.id;
    }

    void revoke_grant(uint64_t grant_id) {
        ayni_grant_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_grant(work.kernel(), grant_id, &info), AYNI_OK);
        const ayni_grant_revoke_v1 cmd{sizeof(cmd), 0, grant_id};
        auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &cmd, info.revision);
        intent.command_id = ++next_command;
        EXPECT_EQ(work.submit(intent).status, AYNI_OK);
    }

    void set_scene(bool enabled) {
        ayni_scene_info_v1 info{};
        info.struct_size = sizeof(info);
        ASSERT_EQ(ayni_get_scene(work.kernel(), text("bench.scene"), &info), AYNI_OK);
        const ayni_scene_set_enabled_v1 cmd{sizeof(cmd), enabled ? 1U : 0U, text("bench.scene"),
                                            enabled ? 1U : 0U, 0};
        auto intent = work.intent(AYNI_INTENT_SCENE_SET_ENABLED, &cmd, info.revision);
        intent.command_id = ++next_command;
        EXPECT_EQ(work.submit(intent).status, AYNI_OK);
    }

    uint64_t create_goal(const char *type_id, const char *param = nullptr) {
        ayni_goal_create_v1 create{};
        create.struct_size = sizeof(create);
        create.owner_instance_id = work.instance(agent_a);
        create.goal_type_version = 1;
        create.goal_type_id = text(type_id);
        if (std::strcmp(type_id, "bg_ephem") == 0) {
            create.trigger = AYNI_TRIGGER_INTERVAL;
            create.trigger_delay_us = 0;
            create.trigger_period_us = AYNI_TRIGGER_PERIOD_MIN_US;
        } else {
            create.trigger = AYNI_TRIGGER_ONCE;
        }
        if (param != nullptr) {
            create.parameters = {reinterpret_cast<const uint8_t *>(param),
                                 static_cast<uint32_t>(std::strlen(param))};
        }
        auto intent = work.intent(AYNI_INTENT_GOAL_CREATE, &create);
        intent.command_id = ++next_command;
        const auto accepted = work.submit(intent);
        EXPECT_EQ(accepted.status, AYNI_OK);
        return accepted.result.object.id;
    }

    void pump(uint32_t rounds = 10) {
        for (uint32_t at = 0; at < rounds; ++at) {
            work.node().sim().complete_storage();
            work.node().step();
            work.node().sim().advance_us(step_tick_us);
        }
        work.node().sim().complete_storage();
        work.node().step();
    }

    ayni_resource_info_v1 resource_info(const char *id) {
        ayni_resource_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_resource(work.kernel(), text(id), &info), AYNI_OK);
        return info;
    }

    static ayni_status_t stage_motor(const char *step_key, int64_t speed,
                                     ayni_policy_output_v1 &output,
                                     ayni_call_preemption_t preemption = AYNI_CALL_PREEMPT_NONE,
                                     uint32_t mode = AYNI_CALL_MODE_FOREGROUND,
                                     const char *binding = "motor.bind") {
        std::array<ayni_value_node_v1, 2> nodes{};
        nodes[0].kind = AYNI_VALUE_OBJECT;
        nodes[0].subtree_end = 2;
        nodes[1].kind = AYNI_VALUE_I64;
        nodes[1].field_id = execution_field_id;
        nodes[1].subtree_end = 2;
        nodes[1].value.integer = speed;
        ayni_call_request_v1 req{sizeof(req),
                                 AYNI_POLICY_REQUEST_CALL,
                                 1,
                                 0,
                                 text(step_key),
                                 text(binding),
                                 {},
                                 {},
                                 {nodes.data(), 2},
                                 mode,
                                 preemption};
        return output.request_call(output.call_context, &req);
    }

    static ayni_status_t stage_soft(const char *step_key, const char *binding, const char *payload,
                                    ayni_policy_output_v1 &output,
                                    ayni_call_preemption_t preemption = AYNI_CALL_PREEMPT_NONE,
                                    uint32_t mode = AYNI_CALL_MODE_FOREGROUND) {
        ayni_value_node_v1 node{};
        node.kind = AYNI_VALUE_TEXT;
        node.subtree_end = 1;
        node.value.text = text(payload);
        ayni_call_request_v1 req{sizeof(req),
                                 AYNI_POLICY_REQUEST_CALL,
                                 1,
                                 0,
                                 text(step_key),
                                 text(binding),
                                 {},
                                 {},
                                 {&node, 1},
                                 mode,
                                 preemption};
        return output.request_call(output.call_context, &req);
    }
};

std::string param_of(const ayni_task_snapshot_v1 &snap) {
    if (!snap.goal_parameters.data || !snap.goal_parameters.size) {
        return {};
    }
    return std::string(reinterpret_cast<const char *>(snap.goal_parameters.data),
                       snap.goal_parameters.size);
}

// Test 1 (ADR-0111): SUPERSEDE preempts same-priority EPHEMERAL physical holder with
// AYNI_REASON_PREEMPTED, inline STOPPED releases resource without quarantine/STATUS polls,
// and successor executes immediately; LOWER does not preempt same-priority foreground.
TEST(Preemption, SamePrioritySupersedePreemptsHeldEphemeralAndLowerWaits) {
    PreemptionFixture fix;
    ayni_task_call_result_v1 a_result{};
    ayni_task_call_result_v1 b_result{};
    ayni_task_call_result_v1 c_result{};

    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        const auto tag = param_of(snap);
        ayni_task_call_result_v1 res{};
        res.struct_size = sizeof(res);
        const auto rc = snap.call_result(snap.call_context, text("m"), &res);
        if (rc == AYNI_NOT_FOUND) {
            const auto policy = tag == "C"   ? AYNI_CALL_PREEMPT_SUPERSEDE
                                : tag == "B" ? AYNI_CALL_PREEMPT_LOWER
                                             : AYNI_CALL_PREEMPT_NONE;
            EXPECT_EQ(PreemptionFixture::stage_motor("m", 40, out, policy), AYNI_OK);
            return AYNI_OK;
        }
        if (tag == "A") {
            a_result = res;
        } else if (tag == "B") {
            b_result = res;
        } else if (tag == "C") {
            c_result = res;
        }
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };

    fix.start();
    const uint64_t g_a = fix.create_goal("fg_ephem", "A");
    fix.pump(6);
    ASSERT_TRUE(fix.motor_exec.active.has_value());
    ASSERT_EQ(fix.motor_exec.submit_calls, 1U);
    const uint64_t call_a = fix.motor_exec.active->call_id;

    // Goal B (same-priority foreground, PREEMPT_LOWER) must NOT preempt Goal A.
    const uint64_t g_b = fix.create_goal("fg_ephem", "B");
    fix.pump(6);
    EXPECT_EQ(fix.motor_exec.cancel_calls, 0U);
    EXPECT_EQ(fix.motor_exec.stop_calls, 0U);
    EXPECT_EQ(fix.motor_exec.submit_calls, 1U);
    EXPECT_EQ(fix.motor_exec.active->call_id, call_a);

    // Goal C (same-priority foreground, PREEMPT_SUPERSEDE) MUST preempt Goal A immediately.
    const uint64_t g_c = fix.create_goal("fg_ephem", "C");
    fix.pump(4);

    // Goal A was cancelled with AYNI_REASON_PREEMPTED, inline stop proof avoided quarantine,
    // and Goal C also superseded older queued Goal B before B submitted.
    EXPECT_GE(fix.motor_exec.cancel_calls, 1U);
    EXPECT_NE(fix.resource_info("motor.actuator").state, AYNI_RESOURCE_QUARANTINED);
    EXPECT_EQ(a_result.outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(a_result.status, AYNI_OK);
    EXPECT_EQ(a_result.reason, AYNI_REASON_PREEMPTED);
    EXPECT_EQ(b_result.outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(b_result.reason, AYNI_REASON_PREEMPTED);
    EXPECT_EQ(fix.work.goal(g_a).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(fix.work.goal(g_b).lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(fix.motor_exec.submit_calls, 2U);
    const uint64_t call_c = fix.motor_exec.active->call_id;
    EXPECT_NE(call_c, call_a);

    // Finish Goal C cleanly; resource returns to FREE with zero quarantine.
    fix.motor_exec.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "c_ok",
                                 true);
    fix.pump(6);
    EXPECT_EQ(c_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(fix.work.goal(g_c).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(fix.resource_info("motor.actuator").state, AYNI_RESOURCE_FREE);
}

// Test 2 (ADR-0111): Foreground AYNI_CALL_PREEMPT_LOWER preempts a held BACKGROUND EPHEMERAL call.
TEST(Preemption, LowerPreemptionPreemptsBackgroundEphemeralCall) {
    PreemptionFixture fix;
    ayni_task_call_result_v1 bg_result{};
    ayni_task_call_result_v1 fg_result{};

    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        const auto tag = param_of(snap);
        ayni_task_call_result_v1 res{};
        res.struct_size = sizeof(res);
        if (snap.call_result(snap.call_context, text("m"), &res) == AYNI_NOT_FOUND) {
            const auto policy = tag == "FG" ? AYNI_CALL_PREEMPT_LOWER : AYNI_CALL_PREEMPT_NONE;
            EXPECT_EQ(PreemptionFixture::stage_motor("m", 30, out, policy), AYNI_OK);
            return AYNI_OK;
        }
        (tag == "BG" ? bg_result : fg_result) = res;
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };

    fix.start();
    const uint64_t g_bg = fix.create_goal("bg_ephem", "BG");
    fix.pump(6);
    ASSERT_EQ(fix.motor_exec.submit_calls, 1U);

    const uint64_t g_fg = fix.create_goal("fg_ephem", "FG");
    fix.pump(8);
    EXPECT_EQ(bg_result.outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(bg_result.reason, AYNI_REASON_PREEMPTED);
    // Interval Goal remains active for future occurrences after its preempted Task completes.
    EXPECT_EQ(fix.work.goal(g_bg).lifecycle, AYNI_GOAL_ACTIVE);
    ASSERT_EQ(fix.motor_exec.submit_calls, 2U);

    fix.motor_exec.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "fg_ok",
                                 true);
    fix.pump(8);
    EXPECT_EQ(fg_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(fix.work.goal(g_fg).lifecycle, AYNI_GOAL_SATISFIED);
}

// Test 3 (ADR-0111): SUPERSEDE NEVER preempts held Tier 2 DURABLE calls or AYNI_CALL_CANCEL_NONE
// calls; the preemptor waits cleanly until the unpreemptable holder completes.
TEST(Preemption, NeverPreemptsDurableOrNonCancellableHolder) {
    PreemptionFixture fix;
    ayni_task_call_result_v1 holder_res{};
    ayni_task_call_result_v1 waiter_res{};

    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        const auto tag = param_of(snap);
        ayni_task_call_result_v1 res{};
        res.struct_size = sizeof(res);
        if (snap.call_result(snap.call_context, text("c"), &res) == AYNI_NOT_FOUND) {
            if (tag == "DUR") {
                EXPECT_EQ(PreemptionFixture::stage_soft("c", "motor.durable", "d", out), AYNI_OK);
            } else if (tag == "NC") {
                EXPECT_EQ(PreemptionFixture::stage_soft("c", "motor.nocancel", "n", out), AYNI_OK);
            } else {
                EXPECT_EQ(PreemptionFixture::stage_soft("c", "work.ephem", "w", out,
                                                        AYNI_CALL_PREEMPT_SUPERSEDE),
                          AYNI_OK);
            }
            return AYNI_OK;
        }
        (tag == "WAIT" ? waiter_res : holder_res) = res;
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };

    fix.start();
    // Part A: DURABLE holder on work.slot is never preempted.
    const uint64_t g_dur = fix.create_goal("fg_durable", "DUR");
    fix.pump(10);
    ASSERT_EQ(fix.soft_provider.in_flight.size(), 1U);
    const uint64_t dur_call = fix.soft_provider.in_flight.front().call_id;

    const uint64_t g_wait1 = fix.create_goal("fg_ephem", "WAIT");
    fix.pump(10);
    EXPECT_EQ(fix.soft_provider.cancel_calls, 0U);
    EXPECT_EQ(fix.soft_provider.submit_calls, 1U);

    ASSERT_TRUE(fix.soft_provider.complete_call(dur_call));
    fix.pump(10);
    EXPECT_EQ(holder_res.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(fix.work.goal(g_dur).lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(fix.soft_provider.submit_calls, 2U);
    ASSERT_TRUE(fix.soft_provider.complete_call(fix.soft_provider.in_flight.front().call_id));
    fix.pump(8);
    EXPECT_EQ(waiter_res.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(fix.work.goal(g_wait1).lifecycle, AYNI_GOAL_SATISFIED);

    // Part B: EPHEMERAL AYNI_CALL_CANCEL_NONE holder is never preempted.
    const uint64_t g_nc = fix.create_goal("fg_ephem", "NC");
    fix.pump(8);
    ASSERT_EQ(fix.soft_provider.in_flight.size(), 1U);
    const uint64_t nc_call = fix.soft_provider.in_flight.front().call_id;

    const uint64_t g_wait2 = fix.create_goal("fg_ephem", "WAIT");
    fix.pump(8);
    EXPECT_EQ(fix.soft_provider.cancel_calls, 0U);
    EXPECT_EQ(fix.soft_provider.submit_calls, 3U);

    ASSERT_TRUE(fix.soft_provider.complete_call(nc_call));
    fix.pump(8);
    EXPECT_EQ(fix.work.goal(g_nc).lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(fix.soft_provider.submit_calls, 4U);
    ASSERT_TRUE(fix.soft_provider.complete_call(fix.soft_provider.in_flight.front().call_id));
    fix.pump(8);
    EXPECT_EQ(fix.work.goal(g_wait2).lifecycle, AYNI_GOAL_SATISFIED);
}

// Test 4 (ADR-0111): Out-of-scope Schema input, disabled Scene, or revoked Grant rejects
// a SUPERSEDE caller BEFORE preemption can touch the currently running EPHEMERAL holder.
TEST(Preemption, NeverBypassesAuthorizationSceneOrSchemaBeforeTouchingVictim) {
    PreemptionFixture fix;
    ayni_status_t bad_schema_stage = AYNI_OK;
    ayni_task_call_result_v1 bad_scene_res{};
    ayni_task_call_result_v1 victim_res{};

    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        const auto tag = param_of(snap);
        ayni_task_call_result_v1 res{};
        res.struct_size = sizeof(res);
        if (tag == "VICTIM") {
            if (snap.call_result(snap.call_context, text("m"), &res) == AYNI_NOT_FOUND) {
                EXPECT_EQ(PreemptionFixture::stage_motor("m", 50, out), AYNI_OK);
                return AYNI_OK;
            }
            victim_res = res;
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        }
        if (tag == "BAD_SCHEMA") {
            bad_schema_stage =
                PreemptionFixture::stage_motor("m", 999, out, AYNI_CALL_PREEMPT_SUPERSEDE);
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        }
        if (tag == "BAD_SCENE") {
            if (snap.call_result(snap.call_context, text("m"), &res) == AYNI_NOT_FOUND) {
                EXPECT_EQ(PreemptionFixture::stage_motor("m", 25, out, AYNI_CALL_PREEMPT_SUPERSEDE,
                                                         AYNI_CALL_MODE_FOREGROUND, "motor.alt"),
                          AYNI_OK);
                return AYNI_OK;
            }
            bad_scene_res = res;
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        }
        if (tag == "BAD_GRANT") {
            EXPECT_EQ(PreemptionFixture::stage_motor("m", 25, out, AYNI_CALL_PREEMPT_SUPERSEDE),
                      AYNI_DENIED);
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        }
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };

    fix.start();
    const uint64_t g_victim = fix.create_goal("fg_ephem", "VICTIM");
    fix.pump(6);
    ASSERT_EQ(fix.motor_exec.submit_calls, 1U);

    // 1) Schema out-of-range speed (999) is rejected at stage without touching victim.
    fix.create_goal("fg_ephem", "BAD_SCHEMA");
    fix.pump(6);
    EXPECT_NE(bad_schema_stage, AYNI_OK);
    EXPECT_EQ(fix.motor_exec.cancel_calls, 0U);
    EXPECT_EQ(fix.motor_exec.stop_calls, 0U);

    // 2) Disabled alt.scene (motor.alt) is refused at execution admission (Task held with
    // AYNI_REASON_EXECUTION_SCENE) BEFORE preemption can touch victim on bench.scene!
    const uint64_t g_bad_scene = fix.create_goal("fg_ephem", "BAD_SCENE");
    fix.pump(6);
    const auto bad_goal = fix.work.goal(g_bad_scene);
    ASSERT_NE(bad_goal.last_task_id, 0U);
    ayni_task_info_v1 bad_task{};
    bad_task.struct_size = sizeof(bad_task);
    ASSERT_EQ(ayni_get_task(fix.work.kernel(), bad_goal.last_task_id, &bad_task), AYNI_OK);
    EXPECT_EQ(bad_task.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(bad_task.hold_status, AYNI_DENIED);
    EXPECT_EQ(bad_task.hold_reason, AYNI_REASON_EXECUTION_SCENE);
    EXPECT_EQ(bad_scene_res.outcome, 0U);
    EXPECT_EQ(fix.motor_exec.cancel_calls, 0U);
    EXPECT_EQ(fix.motor_exec.stop_calls, 0U);
    EXPECT_EQ(fix.motor_exec.submit_calls, 1U);

    // Complete victim cleanly with zero disturbance.
    fix.motor_exec.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "v_ok",
                                 true);
    fix.pump(6);
    EXPECT_EQ(victim_res.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(fix.work.goal(g_victim).lifecycle, AYNI_GOAL_SATISFIED);

    // 3) Revoked motor_grant prevents BAD_GRANT from dispatching motor.
    fix.revoke_grant(fix.motor_grant);
    fix.pump(6);
    const uint32_t submits_before = fix.motor_exec.submit_calls;
    fix.create_goal("fg_ephem", "BAD_GRANT");
    fix.pump(6);
    EXPECT_EQ(fix.motor_exec.submit_calls, submits_before);
}

// Test 5 (ADR-0111): If preempting an EPHEMERAL physical victim fails to produce stop proof,
// Core quarantines the actuator resource, blocks the preemptor, and dispatches only after
// STOP + STATUS confirms AYNI_CONTROL_STOPPED.
TEST(Preemption, StopProofFailureQuarantinesResourceAndBlocksPreemptorUntilConfirmed) {
    PreemptionFixture fix;
    fix.motor_exec.stop_behavior = PreemptTestExecutor::StopBehavior::fail_stop_stay_active;
    fix.motor_exec.allowed_to_stop = false;

    ayni_task_call_result_v1 next_res{};
    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        const auto tag = param_of(snap);
        ayni_task_call_result_v1 res{};
        res.struct_size = sizeof(res);
        if (snap.call_result(snap.call_context, text("m"), &res) == AYNI_NOT_FOUND) {
            const auto policy =
                tag == "NEXT" ? AYNI_CALL_PREEMPT_SUPERSEDE : AYNI_CALL_PREEMPT_NONE;
            EXPECT_EQ(PreemptionFixture::stage_motor("m", 50, out, policy), AYNI_OK);
            return AYNI_OK;
        }
        if (tag == "NEXT") {
            next_res = res;
        }
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };

    fix.start();
    fix.create_goal("fg_ephem", "OLD");
    fix.pump(6);
    ASSERT_EQ(fix.motor_exec.submit_calls, 1U);

    // Preemptor triggers stop; victim abandons without stop proof and stays ACTIVE.
    const uint64_t g_next = fix.create_goal("fg_ephem", "NEXT");
    fix.pump(8);

    // Resource MUST be QUARANTINED and preemptor MUST NOT dispatch yet.
    EXPECT_EQ(fix.resource_info("motor.actuator").state, AYNI_RESOURCE_QUARANTINED);
    EXPECT_EQ(fix.motor_exec.submit_calls, 1U);
    EXPECT_GE(fix.motor_exec.stop_calls, 1U);

    // Now allow hardware stop confirmation; Core's STOP + STATUS poll confirms STOPPED,
    // clears quarantine, and dispatches the waiting preemptor.
    fix.motor_exec.allowed_to_stop = true;
    fix.pump(40);
    ASSERT_EQ(fix.motor_exec.submit_calls, 2U);
    EXPECT_GE(fix.motor_exec.status_calls, 1U);

    fix.motor_exec.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "ok",
                                 true);
    fix.pump(8);
    EXPECT_EQ(next_res.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(fix.work.goal(g_next).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(fix.resource_info("motor.actuator").state, AYNI_RESOURCE_FREE);
}

// Test 6 (ADR-0111): Multi-resource preemption is strictly all-or-none. When Call B needs
// {work.slot, aux.actuator} with SUPERSEDE while work.slot is held by EPHEMERAL Call A and
// aux.actuator is held by unpreemptable DURABLE Call D, Call A is NOT cancelled until Call D
// releases aux.actuator; once Call D completes, Call B preempts Call A and acquires both
// atomically.
TEST(Preemption, MultiResourceAllOrNonePreemptionNeverCancelsPartialVictims) {
    PreemptionFixture fix;
    ayni_task_call_result_v1 a_res{};
    ayni_task_call_result_v1 b_res{};

    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        const auto tag = param_of(snap);
        ayni_task_call_result_v1 res{};
        res.struct_size = sizeof(res);
        if (snap.call_result(snap.call_context, text("c"), &res) == AYNI_NOT_FOUND) {
            if (tag == "A") {
                EXPECT_EQ(PreemptionFixture::stage_soft("c", "work.ephem", "a", out), AYNI_OK);
            } else if (tag == "D") {
                EXPECT_EQ(PreemptionFixture::stage_soft("c", "aux.durable", "d", out), AYNI_OK);
            } else {
                EXPECT_EQ(PreemptionFixture::stage_soft("c", "dual.ephem", "both", out,
                                                        AYNI_CALL_PREEMPT_SUPERSEDE),
                          AYNI_OK);
            }
            return AYNI_OK;
        }
        if (tag == "A") {
            a_res = res;
        } else if (tag == "B") {
            b_res = res;
        }
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };

    fix.start();
    const uint64_t g_a = fix.create_goal("fg_ephem", "A");
    const uint64_t g_d = fix.create_goal("fg_durable", "D");
    fix.pump(10);
    ASSERT_EQ(fix.soft_provider.in_flight.size(), 2U);
    const uint64_t dur_call = fix.soft_provider.in_flight[1].call_id;

    // Submit dual-resource SUPERSEDE Call B. Because aux.actuator is held by DURABLE Call D,
    // all-or-none preemption MUST NOT cancel EPHEMERAL Call A on work.slot.
    const uint64_t g_b = fix.create_goal("fg_ephem", "B");
    fix.pump(10);
    EXPECT_EQ(fix.soft_provider.cancel_calls, 0U);
    EXPECT_EQ(fix.soft_provider.submit_calls, 2U);

    // Complete DURABLE Call D; now only EPHEMERAL Call A conflicts, so Call B immediately
    // preempts Call A (AYNI_REASON_PREEMPTED) and acquires both resources atomically.
    ASSERT_TRUE(fix.soft_provider.complete_call(dur_call));
    fix.pump(10);
    EXPECT_EQ(a_res.outcome, AYNI_CALL_CANCELLED);
    EXPECT_EQ(a_res.reason, AYNI_REASON_PREEMPTED);
    EXPECT_EQ(fix.work.goal(g_a).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(fix.work.goal(g_d).lifecycle, AYNI_GOAL_SATISFIED);
    ASSERT_EQ(fix.soft_provider.in_flight.size(), 1U);
    EXPECT_EQ(fix.resource_info("work.slot").used, 1U);
    EXPECT_EQ(fix.resource_info("aux.actuator").used, 1U);

    ASSERT_TRUE(fix.soft_provider.complete_call(fix.soft_provider.in_flight.front().call_id));
    fix.pump(8);
    EXPECT_EQ(b_res.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(fix.work.goal(g_b).lifecycle, AYNI_GOAL_SATISFIED);
}

// Test 7 (ADR-0111): Invalid preemption forms (enum > SUPERSEDE, MANAGED + preemption,
// invalid ToolLoopConfig::tool_preemption) are rejected explicitly.
TEST(Preemption, RejectsInvalidPreemptionFormsAndStrategyConfig) {
    PreemptionFixture fix;
    ayni_status_t invalid_enum_rc = AYNI_OK;
    ayni_status_t managed_preempt_rc = AYNI_OK;

    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        invalid_enum_rc =
            PreemptionFixture::stage_motor("bad1", 10, out, AYNI_CALL_PREEMPT_SUPERSEDE + 1U);
        managed_preempt_rc = PreemptionFixture::stage_soft(
            "bad2", "motor.durable", "x", out, AYNI_CALL_PREEMPT_SUPERSEDE, AYNI_CALL_MODE_MANAGED);
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };
    fix.start();
    const uint64_t g = fix.create_goal("fg_durable", "BAD");
    fix.pump(10);
    EXPECT_EQ(invalid_enum_rc, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(managed_preempt_rc, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(fix.work.goal(g).lifecycle, AYNI_GOAL_SATISFIED);

    // strategies::ToolLoop rejects invalid tool_preemption during configure().
    ayni::strategies::ToolLoop loop;
    ayni::strategies::ToolLoopConfig cfg;
    cfg.identity.id = text("test.loop");
    cfg.model.kind = cfg.summary.kind = AYNI_POLICY_REQUEST_MODEL;
    cfg.model.binding_id = cfg.summary.binding_id = text("model.bind");
    cfg.limits = {4096, 512, 1024, 512};
    cfg.tool_preemption = AYNI_CALL_PREEMPT_SUPERSEDE + 1U;
    ayni_body_store_v1 dummy_store{};
    dummy_store.struct_size = sizeof(dummy_store);
    dummy_store.write = [](void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *) {
        return AYNI_UNSUPPORTED;
    };
    dummy_store.read = [](void *, const ayni_body_ref_v1 *, uint8_t *, uint32_t) {
        return AYNI_UNSUPPORTED;
    };
    cfg.bodies = &dummy_store;
    size_t ws_bytes = 0, ws_align = 0;
    ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&ws_bytes, &ws_align), AYNI_OK);
    std::vector<uint64_t> backing((ws_bytes + sizeof(uint64_t) - 1U) / sizeof(uint64_t));
    const ayni_workspace_v1 ws{sizeof(ws), 0, backing.data(), backing.size() * sizeof(uint64_t)};
    EXPECT_EQ(loop.configure(cfg, &ws), AYNI_INVALID_ARGUMENT);
}

// Synchronous in-memory FileValuePort counting conversation_root writes to prove zero-Flash
// volatile draft discard on background+discard cancel while preserving durable/retain paths.
struct CountingFileValueStore {
    struct Entry {
        uint64_t generation = 0;
        std::vector<uint8_t> bytes;
    };
    using Key = std::pair<uint64_t, uint64_t>;
    std::map<Key, Entry> values;
    uint32_t root_writes = 0;
    uint32_t total_writes = 0;

    static ayni_submit_result_t on_submit(void *ctx,
                                          const ayni::hosts::storage::FileValueRequest *req) {
        auto &self = *static_cast<CountingFileValueStore *>(ctx);
        using Kind = ayni::hosts::storage::FileValueRequest::Kind;
        using Role = ayni::hosts::storage::FileValueRole;
        const Key key{req->key.owner_instance_id, req->key.object_id};
        ayni::hosts::storage::FileValueResult res{};
        if (req->kind == Kind::read) {
            const auto it = self.values.find(key);
            if (it == self.values.end()) {
                res.status = AYNI_NOT_FOUND;
            } else if (req->size < it->second.bytes.size()) {
                res.status = AYNI_BUFFER_TOO_SMALL;
                res.size = static_cast<uint32_t>(it->second.bytes.size());
            } else {
                if (!it->second.bytes.empty()) {
                    std::memcpy(req->buffer, it->second.bytes.data(), it->second.bytes.size());
                }
                res.status = AYNI_OK;
                res.generation = it->second.generation;
                res.size = static_cast<uint32_t>(it->second.bytes.size());
            }
        } else if (req->kind == Kind::write) {
            ++self.total_writes;
            if (req->role == Role::conversation_root) {
                ++self.root_writes;
            }
            Entry next{req->generation, {}};
            if (req->size) {
                next.bytes.assign(req->data, req->data + req->size);
            }
            self.values[key] = std::move(next);
            res.status = AYNI_OK;
            res.generation = req->generation;
            res.size = req->size;
        } else {
            self.values.erase(key);
            res.status = AYNI_OK;
        }
        req->complete(req->context, res);
        return AYNI_SUBMIT_ACCEPTED;
    }

    ayni::hosts::storage::FileValuePort port() { return {this, on_submit}; }
};

struct SessionHarness {
    WorkNode work;
    CountingFileValueStore files;
    ayni::runtime::FileBodies bodies;
    ayni::strategies::ToolLoop loop;
    ayni::runtime::conversation::Session session;
    MultiStubProvider model_provider;
    std::vector<uint64_t> ws_storage;
    ayni_workspace_v1 ws{};
    uint64_t next_cmd = 1;
    std::array<ayni_schema_node_v1, 2> model_input_schema{};
    ayni_schema_node_v1 text_schema{};
    ayni_business_contract_v1 text_business{};
    ayni_call_capability_v1 model_cap{};
    ayni_strategy_v1 strat{};
    ayni::strategies::Persistence persistence = ayni::strategies::Persistence::background;
    ayni::runtime::conversation::UnfinishedTurn unfinished =
        ayni::runtime::conversation::UnfinishedTurn::discard;

    void start() {
        size_t b1 = 0, a1 = 0, b2 = 0, a2 = 0;
        ASSERT_EQ(ayni::strategies::ToolLoop::workspace_requirements(&b1, &a1), AYNI_OK);
        ASSERT_EQ(ayni::strategies::Conversation::workspace_requirements(&b2, &a2), AYNI_OK);
        const size_t bytes = std::max(b1, b2);
        ws_storage.resize((bytes + sizeof(uint64_t) - 1U) / sizeof(uint64_t));
        ws = {sizeof(ws), 0, ws_storage.data(), ws_storage.size() * sizeof(uint64_t)};
        ASSERT_EQ(bodies.configure(files.port(), 256 * 1024, 1000), AYNI_OK);

        auto &cfg = work.node().config();
        cfg.call_capacity = 8;
        cfg.call_input_max_bytes = 512;
        cfg.call_result_max_bytes = 256;
        cfg.call_capability_capacity = 4;
        cfg.provider_capacity = 4;
        cfg.binding_capacity = 4;
        cfg.resource_request_capacity = 8;
        cfg.goal_type_capacity = 4;
        cfg.goal_capacity = 8;
        cfg.task_capacity = 8;
        cfg.task_step_capacity = 12;
        cfg.task_checkpoint_max_bytes = ayni::strategies::ToolLoop::checkpoint_bytes;
        cfg.goal_parameter_max_bytes = ayni::strategies::ToolLoop::checkpoint_bytes;
        cfg.command_capacity = 32;
        cfg.grant_capacity = 8;
        cfg.memory_capacity = 4;
        cfg.memory_body_max_bytes = 64;
        cfg.memory_dependency_capacity = 4;
        cfg.storage_buffer_bytes = 0;
        work.node().set_bodies(bodies.store());

        work.node().on_create([this](ayni_kernel_t *kernel) {
            const auto owner = owner_desc();
            ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
            const auto agent = agent_desc(agent_a);
            ASSERT_EQ(ayni_register_agent(kernel, &agent), AYNI_OK);

            const ayni_capability_desc_v1 m_auth{sizeof(m_auth), 1, text("chat.model"), 0, 0,
                                                 nullptr};
            ASSERT_EQ(ayni_register_capability(kernel, &m_auth), AYNI_OK);
            model_input_schema = {};
            model_input_schema[0].kind = AYNI_VALUE_OBJECT;
            model_input_schema[0].subtree_end = 2;
            model_input_schema[1].kind = AYNI_VALUE_TEXT;
            model_input_schema[1].field_id = AYNI_MODEL_BODY_FIELD_ID;
            model_input_schema[1].subtree_end = 2;
            model_input_schema[1].flags = AYNI_SCHEMA_REQUIRED | AYNI_SCHEMA_EXTERNAL;
            model_input_schema[1].name = text("body");
            model_input_schema[1].max_count = 65536;
            text_schema = {};
            text_schema.kind = AYNI_VALUE_TEXT;
            text_schema.subtree_end = 1;
            text_schema.max_count = 256;
            text_business = {{model_input_schema.data(), 2}, {&text_schema, 1}, 0, 0, nullptr};
            model_cap = {sizeof(model_cap),
                         1,
                         text("chat.model"),
                         {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_MODEL, 0,
                          AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                          30000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 512, 256, 0, nullptr,
                          nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                         &text_business};
            ASSERT_EQ(ayni_register_call_capability(kernel, &model_cap), AYNI_OK);

            const ayni_provider_v1 mp{sizeof(mp),
                                      1,
                                      text("chat.prov"),
                                      text("chat.ep"),
                                      {},
                                      &model_provider,
                                      1,
                                      0,
                                      &model_cap,
                                      MultiStubProvider::on_submit,
                                      MultiStubProvider::on_cancel,
                                      nullptr,
                                      nullptr};
            ASSERT_EQ(ayni_register_provider(kernel, &mp), AYNI_OK);
            const ayni_binding_v1 mb{sizeof(mb),
                                     1,
                                     text("chat.model"),
                                     text("chat.model"),
                                     1,
                                     1,
                                     text("chat.prov"),
                                     text("chat.ep"),
                                     {},
                                     0,
                                     0,
                                     nullptr};
            ASSERT_EQ(ayni_register_binding(kernel, &mb), AYNI_OK);

            ayni::strategies::ToolLoopConfig lc;
            lc.identity.id = text("chat.loop");
            lc.model.kind = lc.summary.kind = AYNI_POLICY_REQUEST_MODEL;
            lc.model.binding_id = lc.summary.binding_id = text("chat.model");
            lc.limits = {4096, 512, 1024, 512};
            lc.bodies = bodies.store();
            lc.persistence = persistence;
            lc.body_progress = {&bodies, ayni::runtime::FileBodies::progress};
            ASSERT_EQ(loop.configure(lc, &ws), AYNI_OK);
            strat = loop.describe();
            ASSERT_EQ(ayni_register_strategy(kernel, &strat), AYNI_OK);

            ayni_goal_type_v1 gt{};
            gt.struct_size = sizeof(gt);
            gt.version = 1;
            gt.goal_type_id = text("chat.turn");
            gt.strategy_id = strat.strategy_id;
            gt.ctx = &ws;
            gt.validate = ayni::strategies::Conversation::validate_turn;
            gt.export_requirements = [](void *, ayni_bytes_v1,
                                        const ayni_goal_requirement_sink_v1 *sink) {
                const ayni_goal_requirement_v1 req{
                    sizeof(req), 1, text("chat.model"), {sizeof(ayni_scope_v1), 0, nullptr}};
                return sink->emit(sink->ctx, &req);
            };
            gt.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
            gt.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
            gt.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT, 0};
            gt.context_mode = AYNI_GOAL_CONTEXT_AGENT;
            ASSERT_EQ(ayni_register_goal_type(kernel, &gt), AYNI_OK);
        });

        work.run();
        const ayni_grant_create_v1 grant{sizeof(grant),
                                         1,
                                         text("chat.model"),
                                         work.instance(agent_a),
                                         0,
                                         AYNI_GRANT_PERMANENT,
                                         0,
                                         0,
                                         {sizeof(ayni_scope_v1), 0, nullptr}};
        auto g_intent = work.intent(AYNI_INTENT_GRANT_CREATE, &grant);
        g_intent.command_id = ++next_cmd;
        ASSERT_EQ(work.submit(g_intent).status, AYNI_OK);
        pump(5);

        ayni_context_command_v1 open_ctx{};
        open_ctx.struct_size = sizeof(open_ctx);
        open_ctx.owner_instance_id = work.instance(agent_a);
        auto c_intent = work.intent(AYNI_INTENT_CONTEXT_OPEN, &open_ctx);
        c_intent.command_id = ++next_cmd;
        ASSERT_EQ(work.submit(c_intent).status, AYNI_OK);
        pump(10);

        ayni::runtime::conversation::Config sc;
        sc.kernel = work.kernel();
        sc.session = work.session();
        sc.owner_instance = work.instance(agent_a);
        sc.goal_type = text("chat.turn");
        sc.workspace = &ws;
        sc.files = files.port();
        sc.bodies = bodies.collection();
        sc.unfinished = unfinished;
        sc.persistence = persistence;
        sc.commands = {this,
                       [](void *ctx, ayni_intent_kind_t kind, const void *body,
                          uint32_t revision) -> ayni::Submission {
                           auto &self = *static_cast<SessionHarness *>(ctx);
                           auto intent = self.work.intent(kind, body, revision);
                           intent.command_id = ++self.next_cmd;
                           const auto accepted = self.work.submit(intent);
                           ayni::Submission out{};
                           out.status = accepted.status;
                           out.result = accepted.result;
                           out.error = accepted.error;
                           return out;
                       }};
        ASSERT_EQ(session.configure(sc), AYNI_OK);
        ASSERT_TRUE(until([&] { return session.ready(); }));
    }

    void pump(uint32_t rounds) {
        for (uint32_t at = 0; at < rounds; ++at) {
            work.node().sim().complete_storage();
            work.node().step();
            work.node().sim().advance_us(step_tick_us);
        }
    }

    template <typename Pred> bool until(Pred pred, uint32_t limit = 80) {
        for (uint32_t at = 0; at < limit; ++at) {
            if (pred()) {
                return true;
            }
            const auto st = session.poll();
            if (st != AYNI_OK && st != AYNI_BUSY && st != AYNI_NOT_READY) {
                ADD_FAILURE() << "unexpected session.poll() status=" << st
                              << " held=" << session.held();
                return false;
            }
            work.node().sim().complete_storage();
            work.node().step();
            work.node().sim().advance_us(step_tick_us);
        }
        return pred();
    }
};

// Test 8 (ADR-0111): Background+discard cancels an active turn in RAM with ZERO conversation_root
// Flash writes, preserves shared historical user bodies when the cancelled prompt repeats an
// earlier turn's text, and immediately allows the next turn to complete and publish.
TEST(Preemption, BackgroundDiscardCancelsWithZeroRootWritesAndPreservesSharedHistory) {
    SessionHarness h;
    h.start();
    const uint32_t writes_after_init = h.files.root_writes;
    ASSERT_EQ(writes_after_init, 1U);

    // Turn 1: complete and publish ("forward 1s").
    ASSERT_EQ(h.session.begin(text("forward 1s")), AYNI_OK);
    ASSERT_TRUE(h.until([&] { return !h.model_provider.in_flight.empty(); }));
    ASSERT_TRUE(h.model_provider.complete_call(h.model_provider.in_flight.front().call_id, "r1"));
    ayni::runtime::conversation::TurnResult r1{};
    ASSERT_TRUE(h.until([&] { return h.session.take_result(r1); }));
    ASSERT_TRUE(r1.published);
    ASSERT_EQ(h.session.record()->last_round, 1U);
    const uint32_t writes_after_turn1 = h.files.root_writes;
    ASSERT_EQ(writes_after_turn1, 2U);

    // Turn 2: repeat exact same text ("forward 1s") so its body alias matches Turn 1's round.user,
    // then cancel mid-turn (simulating live barge-in).
    ASSERT_EQ(h.session.begin(text("forward 1s")), AYNI_OK);
    ASSERT_TRUE(h.until([&] { return !h.model_provider.in_flight.empty(); }));
    ASSERT_EQ(h.session.cancel(), AYNI_OK);
    ayni::runtime::conversation::TurnResult r2{};
    ASSERT_TRUE(h.until([&] { return h.session.take_result(r2); }));
    EXPECT_FALSE(r2.published);
    EXPECT_TRUE(h.session.ready());
    // Zero conversation_root Flash writes for discarding the unpersisted RAM draft!
    EXPECT_EQ(h.files.root_writes, writes_after_turn1);
    EXPECT_EQ(h.session.record()->last_round, 1U);
    EXPECT_EQ(h.session.record()->draft.id, 0U);

    // Turn 1's shared user input body MUST still be readable (not erroneously released!).
    std::vector<ayni::strategies::Round> rounds;
    ASSERT_TRUE(h.until([&] {
        return ayni::strategies::Conversation::rounds(*h.bodies.store(), *h.session.record(),
                                                      rounds) == AYNI_OK;
    }));
    ASSERT_EQ(rounds.size(), 1U);
    EXPECT_EQ(rounds[0].user, "forward 1s");
    EXPECT_EQ(rounds[0].answer, "r1");

    // Turn 3 starts immediately and publishes normally.
    ASSERT_EQ(h.session.begin(text("turn left")), AYNI_OK);
    ASSERT_TRUE(h.until([&] { return !h.model_provider.in_flight.empty(); }));
    ASSERT_TRUE(h.model_provider.complete_call(h.model_provider.in_flight.front().call_id, "r3"));
    ayni::runtime::conversation::TurnResult r3{};
    ASSERT_TRUE(h.until([&] { return h.session.take_result(r3); }));
    EXPECT_TRUE(r3.published);
    EXPECT_EQ(h.session.record()->last_round, 2U);
    EXPECT_EQ(h.files.root_writes, writes_after_turn1 + 1U);
}

// Test 9 (ADR-0111): Regression guard — Persistence::background + UnfinishedTurn::retain still
// saves the cancelled draft durably and requires explicit abandon().
TEST(Preemption, BackgroundRetainStillPersistsDraftOnCancel) {
    SessionHarness h;
    h.unfinished = ayni::runtime::conversation::UnfinishedTurn::retain;
    h.start();
    const uint32_t writes_after_init = h.files.root_writes;

    ASSERT_EQ(h.session.begin(text("keep me")), AYNI_OK);
    ASSERT_TRUE(h.until([&] { return !h.model_provider.in_flight.empty(); }));
    ASSERT_EQ(h.session.cancel(), AYNI_OK);
    ayni::runtime::conversation::TurnResult r{};
    ASSERT_TRUE(h.until([&] { return h.session.take_result(r); }));
    EXPECT_FALSE(r.published);
    EXPECT_FALSE(h.session.ready());
    // Retain mode durably wrote the retained draft to conversation_root.
    EXPECT_EQ(h.files.root_writes, writes_after_init + 1U);
    EXPECT_NE(h.session.record()->draft.id, 0U);

    // Explicit abandon clears the persisted draft durably (writes conversation_root once more).
    ASSERT_EQ(h.session.abandon(), AYNI_OK);
    ASSERT_TRUE(h.until([&] { return h.session.ready(); }));
    EXPECT_EQ(h.files.root_writes, writes_after_init + 2U);
    EXPECT_EQ(h.session.record()->draft.id, 0U);
}

// Test 10 (ADR-0110/ADR-0111): Context erasure cleanly scrubs completed RAM-only EPHEMERAL calls
// owned by AYNI_GOAL_CONTEXT_AGENT tasks without emitting unreplayable WAL records or hitting
// RESTRICTED, and subsequent turns publish normally.
TEST(Preemption, ContextEraseScrubsEphemeralCallAndAllowsNextTurn) {
    SessionHarness h;
    h.start();

    ASSERT_EQ(h.session.begin(text("secret prompt")), AYNI_OK);
    ASSERT_TRUE(h.until([&] { return !h.model_provider.in_flight.empty(); }));
    ASSERT_TRUE(h.model_provider.complete_call(h.model_provider.in_flight.front().call_id, "ok"));
    ayni::runtime::conversation::TurnResult r1{};
    ASSERT_TRUE(h.until([&] { return h.session.take_result(r1); }));
    ASSERT_TRUE(r1.published);

    ASSERT_EQ(h.session.forget(AYNI_CONTEXT_ERASE_ALL), AYNI_OK);
    ASSERT_TRUE(h.until([&] { return h.session.ready(); }, 200));
    EXPECT_EQ(h.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(h.session.record()->round_count, 0U);

    // Next turn after context erasure runs and publishes cleanly.
    ASSERT_EQ(h.session.begin(text("fresh turn")), AYNI_OK);
    ASSERT_TRUE(h.until([&] { return !h.model_provider.in_flight.empty(); }));
    ASSERT_TRUE(h.model_provider.complete_call(h.model_provider.in_flight.front().call_id, "r2"));
    ayni::runtime::conversation::TurnResult r2{};
    ASSERT_TRUE(h.until([&] { return h.session.take_result(r2); }));
    EXPECT_TRUE(r2.published);
    EXPECT_EQ(h.session.record()->round_count, 1U);
}

} // namespace
