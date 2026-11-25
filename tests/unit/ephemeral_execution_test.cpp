// ADR-0110 / contracts/storage/records.md: Phase 1 Tier 0 EPHEMERAL execution,
// CANCEL_ON_REBOOT boot-reset cancellation, zero-WAL volatile turn progress,
// colocated inline stop proof, fail-closed quarantine fallback, live authority
// revocation, and mixed Tier 0 + Tier 2 execution within one Task.

#include "durability/batch/batch_codec.h"
#include "foundation/crc32.h"
#include "foundation/little_endian.h"
#include "kernel_config_fixture.h"
#include "work/call/call_records.h"
#include "work/goal/goal_records.h"
#include "work_node.h"
#include "workspace_fixture.h"

#include "journal_test_support.h"
#include <ayni/audit.h>
#include <ayni/calls.h>
#include <ayni/execution.h>
#include <ayni/observations.h>
#include <ayni/scenes.h>
#include <ayni/storage_format.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace ayni::test;

constexpr uint64_t step_tick_us = 10000;
constexpr uint32_t lease_us = 200000;
constexpr uint32_t poll_period_us = 50000;
constexpr uint64_t control_domain_id = 42;
constexpr uint32_t execution_field_id = 1;

// Minimal colocated physical executor + provider stub that gives tests exact
// control over flags, inline stop proof payloads, forged proofs, and STOP/STATUS counts.
struct InlineTestExecutor {
    enum class ProofMode {
        valid_stopped,
        omit,
        wrong_permit_id,
        still_active,
    };

    uint32_t flags = AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF;
    ProofMode proof_mode = ProofMode::valid_stopped;
    bool auto_complete = true;
    uint32_t challenge_calls = 0;
    uint32_t stop_calls = 0;
    uint32_t status_calls = 0;
    uint32_t submit_calls = 0;
    uint32_t cancel_calls = 0;
    uint64_t next_nonce = 100;

    struct Active {
        ayni_attempt_ref_v1 attempt{};
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

    static ayni_status_t on_challenge(void *ctx, ayni_attempt_ref_v1 request,
                                      const ayni_permit_epochs_v1 *,
                                      ayni_executor_completion_t *completion) {
        auto &self = *static_cast<InlineTestExecutor *>(ctx);
        ++self.challenge_calls;
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
        auto &self = *static_cast<InlineTestExecutor *>(ctx);
        ++self.stop_calls;
        if (self.active && self.active->permit_id == permit_id) {
            self.active->stopped = true;
        }
        if (self.last_permit_id == permit_id) {
            self.last_stopped = true;
        }
        ayni_executor_response_v1 resp{};
        resp.struct_size = sizeof(resp);
        resp.kind = AYNI_EXECUTOR_STOP;
        resp.request = request;
        resp.status = AYNI_OK;
        return ayni_executor_post(completion, &resp);
    }

    static ayni_status_t on_status(void *ctx, ayni_attempt_ref_v1 request, uint64_t permit_id,
                                   ayni_executor_completion_t *completion) {
        auto &self = *static_cast<InlineTestExecutor *>(ctx);
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
        const bool is_stopped = match_active ? self.active->stopped : self.last_stopped;
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
        auto &self = *static_cast<InlineTestExecutor *>(ctx);
        ++self.submit_calls;
        if (!request || !request->execution_permit || !completion) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.active = Active{request->attempt,
                             request->execution_permit->permit_id,
                             request->execution_permit->operation_id,
                             completion,
                             false,
                             false};
        self.last_permit_id = request->execution_permit->permit_id;
        self.last_operation_id = request->execution_permit->operation_id;
        self.last_stopped = false;
        if (self.auto_complete) {
            self.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "moved");
        }
        return AYNI_SUBMIT_ACCEPTED;
    }

    static ayni_submit_result_t on_cancel(void *ctx, ayni_attempt_ref_v1) {
        auto &self = *static_cast<InlineTestExecutor *>(ctx);
        ++self.cancel_calls;
        return AYNI_SUBMIT_ACCEPTED;
    }

    void finish_active(ayni_call_outcome_t outcome, ayni_status_t status, ayni_call_effect_t effect,
                       const char *reply) {
        if (!active || active->posted) {
            return;
        }
        active->posted = true;
        active->stopped = (proof_mode == ProofMode::valid_stopped);
        last_stopped = active->stopped;
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
        if (proof_mode != ProofMode::omit && outcome != AYNI_CALL_ABANDONED) {
            inline_status.struct_size = sizeof(inline_status);
            inline_status.target = AYNI_TARGET_UNVERIFIED;
            inline_status.control.struct_size = sizeof(inline_status.control);
            inline_status.control.permit_id = proof_mode == ProofMode::wrong_permit_id
                                                  ? active->permit_id + 999U
                                                  : active->permit_id;
            inline_status.control.operation_id = active->operation_id;
            inline_status.control.phase =
                proof_mode == ProofMode::still_active ? AYNI_CONTROL_ACTIVE : AYNI_CONTROL_STOPPED;
            inline_status.control.stop_cause = proof_mode == ProofMode::still_active
                                                   ? AYNI_CONTROL_STOP_NONE
                                                   : AYNI_CONTROL_STOP_REQUESTED;
            result.execution_status = &inline_status;
        }
        EXPECT_EQ(ayni_provider_post(active->completion, &result), AYNI_OK);
    }

    ayni_executor_v1 ports() {
        return {sizeof(ayni_executor_v1),
                1,
                text("test.executor"),
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
                flags,
                0};
    }
};

// Minimal synchronous software/model provider that completes immediately or on demand.
struct InlineStubProvider {
    bool auto_complete = true;
    uint32_t submit_calls = 0;
    const char *reply_text = "ok";
    std::optional<ayni_attempt_ref_v1> last_attempt;
    ayni_provider_completion_t *last_completion = nullptr;

    static ayni_submit_result_t on_submit(void *ctx, const ayni_provider_request_v1 *request,
                                          ayni_provider_completion_t *completion) {
        auto &self = *static_cast<InlineStubProvider *>(ctx);
        ++self.submit_calls;
        self.last_attempt = request->attempt;
        self.last_completion = completion;
        if (self.auto_complete) {
            self.complete();
        }
        return AYNI_SUBMIT_ACCEPTED;
    }

    static ayni_submit_result_t on_cancel(void *, ayni_attempt_ref_v1) {
        return AYNI_SUBMIT_ACCEPTED;
    }

    void complete() {
        if (!last_attempt || !last_completion) {
            return;
        }
        ayni_provider_result_v1 result{};
        result.struct_size = sizeof(result);
        result.outcome = AYNI_CALL_COMPLETED;
        result.attempt = *last_attempt;
        result.status = AYNI_OK;
        result.effect = AYNI_CALL_EFFECT_COMPLETE;
        result.result = text(reply_text);
        EXPECT_EQ(ayni_provider_post(last_completion, &result), AYNI_OK);
        last_attempt.reset();
        last_completion = nullptr;
    }
};

// Shared fixture wiring one owner, one agent, RESUME and CANCEL_ON_REBOOT GoalTypes,
// an EPHEMERAL physical motor binding, an EPHEMERAL model binding, and a DURABLE tool binding.
struct EphemeralFixture {
    WorkNode work;
    InlineTestExecutor motor_exec;
    InlineStubProvider model_provider;
    InlineStubProvider durable_provider;
    uint64_t next_command = 1;
    uint64_t motor_grant_id = 0;
    uint64_t model_grant_id = 0;
    uint64_t durable_grant_id = 0;
    bool on_demand_tools = false;
    bool single_batch_journal = false;
    uint32_t max_turns = AYNI_MAX_TURNS_DEFAULT;

    ayni_call_resource_v1 motor_resource{text("motor.actuator"), 1, 0};
    ayni_call_resource_v1 model_resource{text("model.slot"), 1, 0};
    std::array<ayni_schema_node_v1, 2> motor_input_schema{};
    ayni_schema_node_v1 text_schema{};
    ayni_authority_projection_v1 motor_projection{};
    ayni_business_contract_v1 motor_business{};
    ayni_business_contract_v1 text_business{};
    ayni_call_capability_v1 motor_cap{};
    ayni_call_capability_v1 model_cap{};
    ayni_call_capability_v1 durable_cap{};
    std::array<ayni_tool_ref_v1, 2> goal_tools{};

    void configure_node() {
        auto &cfg = work.node().config();
        cfg.call_capacity = 8;
        cfg.call_input_max_bytes = 256;
        cfg.call_result_max_bytes = 128;
        cfg.call_capability_capacity = 4;
        cfg.provider_capacity = 4;
        cfg.binding_capacity = 4;
        cfg.resource_capacity = 4;
        cfg.resource_request_capacity = 8;
        cfg.goal_type_capacity = 4;
        cfg.task_step_capacity = 12;
        cfg.task_checkpoint_max_bytes = 64;
        cfg.max_turns = max_turns;
        cfg.scene_capacity = 2;
        cfg.observation_source_capacity = 2;
        cfg.executor_capacity = 2;
        cfg.execution_binding_capacity = 2;
        cfg.command_capacity = 16;
        cfg.storage_buffer_bytes = single_batch_journal ? smallest_storage_buffer(cfg) : 0;
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

        // Capabilities: motor.act (physical, 1 int param), test.model (model), test.durable (tool).
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
        const ayni_capability_desc_v1 model_auth{
            sizeof(ayni_capability_desc_v1), 1, text("test.model"), 0, 0, nullptr};
        const ayni_capability_desc_v1 durable_auth{
            sizeof(ayni_capability_desc_v1), 1, text("test.durable"), 0, 0, nullptr};
        ASSERT_EQ(ayni_register_capability(kernel, &motor_auth), AYNI_OK);
        ASSERT_EQ(ayni_register_capability(kernel, &model_auth), AYNI_OK);
        ASSERT_EQ(ayni_register_capability(kernel, &durable_auth), AYNI_OK);

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
                      AYNI_CALL_RESULT_TEXT, 128, 64, 1, &motor_resource, nullptr,
                      AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                     &motor_business};
        model_cap = {sizeof(model_cap),
                     1,
                     text("test.model"),
                     {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_MODEL, 0,
                      AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                      30000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 64, 1,
                      &model_resource, nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                     &text_business};
        durable_cap = {sizeof(durable_cap),
                       1,
                       text("test.durable"),
                       {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                        AYNI_CALL_NOT_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_REQUEST, 0,
                        30000, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT, 128, 64, 0, nullptr,
                        nullptr, AYNI_CALL_DURABILITY_DURABLE, 0},
                       &text_business};
        ASSERT_EQ(ayni_register_call_capability(kernel, &motor_cap), AYNI_OK);
        ASSERT_EQ(ayni_register_call_capability(kernel, &model_cap), AYNI_OK);
        ASSERT_EQ(ayni_register_call_capability(kernel, &durable_cap), AYNI_OK);

        // Resources.
        const ayni_resource_desc_v1 motor_res{sizeof(motor_res),
                                              AYNI_RESOURCE_EXCLUSIVE,
                                              text("motor.actuator"),
                                              1,
                                              AYNI_RESOURCE_ACTUATOR,
                                              0,
                                              0,
                                              nullptr,
                                              {}};
        const ayni_resource_desc_v1 model_res{sizeof(model_res),
                                              AYNI_RESOURCE_EXCLUSIVE,
                                              text("model.slot"),
                                              1,
                                              0,
                                              0,
                                              0,
                                              nullptr,
                                              {}};
        ASSERT_EQ(ayni_register_resource(kernel, &motor_res), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, motor_res.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);
        ASSERT_EQ(ayni_register_resource(kernel, &model_res), AYNI_OK);
        ASSERT_EQ(ayni_set_resource_health(kernel, model_res.resource_id, AYNI_RESOURCE_ONLINE),
                  AYNI_OK);

        // Providers & Bindings.
        const ayni_provider_v1 mp{sizeof(mp),
                                  1,
                                  text("motor.prov"),
                                  text("motor.ep"),
                                  {},
                                  &motor_exec,
                                  1,
                                  0,
                                  &motor_cap,
                                  InlineTestExecutor::on_submit,
                                  InlineTestExecutor::on_cancel,
                                  nullptr,
                                  nullptr};
        const ayni_provider_v1 lp{sizeof(lp),
                                  1,
                                  text("model.prov"),
                                  text("model.ep"),
                                  {},
                                  &model_provider,
                                  1,
                                  0,
                                  &model_cap,
                                  InlineStubProvider::on_submit,
                                  InlineStubProvider::on_cancel,
                                  nullptr,
                                  nullptr};
        const ayni_provider_v1 dp{sizeof(dp),
                                  1,
                                  text("durable.prov"),
                                  text("durable.ep"),
                                  {},
                                  &durable_provider,
                                  1,
                                  0,
                                  &durable_cap,
                                  InlineStubProvider::on_submit,
                                  InlineStubProvider::on_cancel,
                                  nullptr,
                                  nullptr};
        ASSERT_EQ(ayni_register_provider(kernel, &mp), AYNI_OK);
        ASSERT_EQ(ayni_register_provider(kernel, &lp), AYNI_OK);
        ASSERT_EQ(ayni_register_provider(kernel, &dp), AYNI_OK);

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
        const ayni_binding_v1 lb{sizeof(lb),
                                 1,
                                 text("model.bind"),
                                 text("test.model"),
                                 1,
                                 1,
                                 text("model.prov"),
                                 text("model.ep"),
                                 {},
                                 0,
                                 0,
                                 nullptr};
        const ayni_binding_v1 db{sizeof(db),
                                 1,
                                 text("durable.bind"),
                                 text("test.durable"),
                                 1,
                                 1,
                                 text("durable.prov"),
                                 text("durable.ep"),
                                 {},
                                 0,
                                 0,
                                 nullptr};
        ASSERT_EQ(ayni_register_binding(kernel, &mb), AYNI_OK);
        ASSERT_EQ(ayni_register_binding(kernel, &lb), AYNI_OK);
        ASSERT_EQ(ayni_register_binding(kernel, &db), AYNI_OK);

        // Observation source, Scene, Executor, ExecutionBinding for motor.bind.
        const ayni_observation_source_v1 obs_src{sizeof(obs_src),
                                                 1,
                                                 text("motor.ready"),
                                                 AYNI_OBSERVATION_BOOL,
                                                 AYNI_OBSERVATION_MEASURED,
                                                 text("bool"),
                                                 60000000};
        ASSERT_EQ(ayni_register_observation_source(kernel, &obs_src), AYNI_OK);

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
        ASSERT_EQ(ayni_register_scene(kernel, &scene_desc), AYNI_OK);

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
        ASSERT_EQ(ayni_register_execution_binding(kernel, &exec_bind), AYNI_OK);

        // GoalTypes: "resume_goal" (RESUME) and "ephemeral_goal" (CANCEL_ON_REBOOT).
        goal_tools = {{{text("motor.bind"), 1, AYNI_TOOL_ROLE_CAPABILITY},
                       {text("durable.bind"), 1, AYNI_TOOL_ROLE_CAPABILITY}}};
        auto type = goal_type_desc("resume_goal", 1);
        type.export_requirements = on_demand_tools ? emit_model_only : emit_all_requirements;
        if (on_demand_tools) {
            type.tools = goal_tools.data();
            type.tool_count = static_cast<uint32_t>(goal_tools.size());
            type.tool_authorization = AYNI_TOOLS_ON_DEMAND;
        }
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);

        type.goal_type_id = text("ephemeral_goal");
        type.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT, 0};
        ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
    }

    static ayni_status_t emit_model_only(void *, ayni_bytes_v1,
                                         const ayni_goal_requirement_sink_v1 *sink) {
        const ayni_goal_requirement_v1 req{
            sizeof(req), 1, text("test.model"), {sizeof(ayni_scope_v1), 0, nullptr}};
        return sink->emit(sink->ctx, &req);
    }

    static ayni_status_t emit_all_requirements(void *, ayni_bytes_v1,
                                               const ayni_goal_requirement_sink_v1 *sink) {
        const ayni_goal_requirement_v1 m_req{
            sizeof(m_req), 1, text("test.model"), {sizeof(ayni_scope_v1), 0, nullptr}};
        auto rc = sink->emit(sink->ctx, &m_req);
        if (rc != AYNI_OK) {
            return rc;
        }
        const ayni_goal_requirement_v1 d_req{
            sizeof(d_req), 1, text("test.durable"), {sizeof(ayni_scope_v1), 0, nullptr}};
        rc = sink->emit(sink->ctx, &d_req);
        if (rc != AYNI_OK) {
            return rc;
        }
        ayni_scope_field_v1 speed_field{};
        speed_field.struct_size = sizeof(speed_field);
        speed_field.field_id = execution_field_id;
        speed_field.allowed.struct_size = sizeof(speed_field.allowed);
        speed_field.allowed.kind = AYNI_AUTHORITY_I64;
        speed_field.allowed.unit = text("pct");
        speed_field.allowed.values.integer = {-100, 100};
        const ayni_goal_requirement_v1 a_req{
            sizeof(a_req), 1, text("motor.act"), {sizeof(ayni_scope_v1), 1, &speed_field}};
        return sink->emit(sink->ctx, &a_req);
    }

    void start() {
        configure_node();
        work.run();
        motor_grant_id = create_grant("motor.act", true);
        if (single_batch_journal) {
            pump(3);
        }
        model_grant_id = create_grant("test.model", false);
        if (single_batch_journal) {
            pump(3);
        }
        durable_grant_id = create_grant("test.durable", false);
        if (single_batch_journal) {
            pump(3);
        }
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

    uint64_t create_goal(const char *type_id) {
        ayni_goal_create_v1 create{};
        create.struct_size = sizeof(create);
        create.owner_instance_id = work.instance(agent_a);
        create.goal_type_version = 1;
        create.goal_type_id = text(type_id);
        create.trigger = AYNI_TRIGGER_ONCE;
        auto intent = work.intent(AYNI_INTENT_GOAL_CREATE, &create);
        intent.command_id = ++next_command;
        const auto accepted = work.submit(intent);
        EXPECT_EQ(accepted.status, AYNI_OK);
        return accepted.result.object.id;
    }

    void pump(uint32_t rounds, bool complete_io = true) {
        for (uint32_t at = 0; at < rounds; ++at) {
            if (complete_io) {
                work.node().sim().complete_storage();
            }
            work.node().step();
            work.node().sim().advance_us(step_tick_us);
        }
        if (complete_io) {
            work.node().sim().complete_storage();
            work.node().step();
        }
    }

    static ayni_status_t stage_motor_call(const char *step_key, int64_t speed,
                                          ayni_policy_output_v1 &output,
                                          uint32_t mode = AYNI_CALL_MODE_FOREGROUND) {
        std::array<ayni_value_node_v1, 2> nodes{};
        nodes[0].kind = AYNI_VALUE_OBJECT;
        nodes[0].subtree_end = 2;
        nodes[1].kind = AYNI_VALUE_I64;
        nodes[1].field_id = execution_field_id;
        nodes[1].subtree_end = 2;
        nodes[1].value.integer = speed;
        ayni_call_request_v1 req{
            sizeof(req),        AYNI_POLICY_REQUEST_CALL, 1,    0, text(step_key),
            text("motor.bind"), {nodes.data(), 2},        mode, 0};
        return output.request_call(output.call_context, &req);
    }

    static ayni_status_t stage_text_call(ayni_policy_intent_kind_t kind, const char *step_key,
                                         const char *binding, const char *payload,
                                         ayni_policy_output_v1 &output) {
        ayni_value_node_v1 node{};
        node.kind = AYNI_VALUE_TEXT;
        node.subtree_end = 1;
        node.value.text = text(payload);
        ayni_call_request_v1 req{sizeof(req),
                                 kind,
                                 1,
                                 0,
                                 text(step_key),
                                 text(binding),
                                 {&node, 1},
                                 AYNI_CALL_MODE_FOREGROUND,
                                 0};
        return output.request_call(output.call_context, &req);
    }
};

// Test 1 (ADR-0110 / contracts/storage/records.md):
// Schema 12 cleanly rejects legacy schema 11 volumes, preserves work_profile.recovery
// in GOAL records, and refuses to decode EPHEMERAL durability from persisted CALL records.
TEST(EphemeralExecution, Schema12RejectsSchema11VolumeAndValidatesRecordCodecs) {
    EXPECT_EQ(AYNI_STORAGE_SCHEMA_CURRENT, 12U);
    EXPECT_GE(AYNI_ABI_MINOR, 59U);
    EXPECT_EQ(ayni::work::goal_record_fixed_size, 144U);
    EXPECT_EQ(ayni::work::call_record_fixed_size, 452U);

    // Build a valid schema 11 boot batch and confirm BootSequence refuses it cleanly.
    Node node;
    std::array<uint8_t, 64> raw{};
    ayni::durability::BatchIdentity id{1, 1, 1};
    ayni::durability::BatchWriter writer(raw.data(), static_cast<uint32_t>(raw.size()), id);
    const std::size_t size = writer.finish();
    ASSERT_GT(size, ayni::durability::batch_header_size);
    ayni::foundation::store_le16(raw.data() + 4, AYNI_STORAGE_SCHEMA_CURRENT - 1U);
    const std::size_t crc_at = ayni::durability::batch_header_size - 4U;
    ayni::foundation::store_le32(raw.data() + crc_at, ayni::foundation::crc32(raw.data(), crc_at));
    node.sim().log().push_back({1, {raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(size)}});
    node.create_and_start();
    for (int i = 0; i < 8 && node.lifecycle() == AYNI_LIFECYCLE_RECOVERING; ++i) {
        node.sim().complete_storage();
        node.step();
    }
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_LOG_UNSUPPORTED);

    // GOAL record round-trip preserves CANCEL_ON_REBOOT.
    ayni::work::Goal goal{};
    goal.goal_id = 0x100000001ULL;
    goal.owner_instance_id = 0x100000002ULL;
    goal.goal_type_version = 1;
    ASSERT_TRUE(goal.goal_type_id.assign(text("ephemeral_goal")));
    goal.trigger.kind = AYNI_TRIGGER_ONCE;
    goal.work_profile = {AYNI_WORK_FOREGROUND, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT, 0};
    std::vector<uint8_t> encoded(ayni::work::goal_record_size(goal));
    ayni::work::write_goal_record(goal, {}, encoded.data(), {});
    ayni::work::GoalRecord decoded{};
    ASSERT_TRUE(ayni::work::read_goal_record(encoded.data(), static_cast<uint32_t>(encoded.size()),
                                             decoded));
    EXPECT_EQ(decoded.goal.work_profile.recovery, AYNI_GOAL_RECOVERY_CANCEL_ON_REBOOT);
    EXPECT_TRUE(decoded.goal.work_profile.cancel_on_reboot());
}

// Test 2 (ADR-0110):
// EPHEMERAL calls are refused on RESUME Goals, on subscriptions/queryable contracts,
// and in AYNI_CALL_MODE_MANAGED mode.
TEST(EphemeralExecution, EphemeralRequiresCancelOnRebootGoalAndValidContract) {
    EphemeralFixture fix;
    ayni_status_t staged_resume_motor = AYNI_OK;
    ayni_status_t staged_resume_model = AYNI_OK;
    ayni_status_t staged_managed_ephemeral = AYNI_OK;
    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        const auto type = string_of(snap.goal_type_id);
        if (type == "resume_goal") {
            staged_resume_motor = EphemeralFixture::stage_motor_call("m1", 30, out);
            staged_resume_model = EphemeralFixture::stage_text_call(AYNI_POLICY_REQUEST_MODEL, "l1",
                                                                    "model.bind", "hi", out);
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        }
        staged_managed_ephemeral =
            EphemeralFixture::stage_motor_call("m2", 30, out, AYNI_CALL_MODE_MANAGED);
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };
    fix.start();
    const uint64_t g_resume = fix.create_goal("resume_goal");
    const uint64_t g_ephem = fix.create_goal("ephemeral_goal");
    fix.pump(25);
    EXPECT_EQ(staged_resume_motor, AYNI_DENIED);
    EXPECT_EQ(staged_resume_model, AYNI_DENIED);
    EXPECT_EQ(staged_managed_ephemeral, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(fix.work.goal(g_resume).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(fix.work.goal(g_ephem).lifecycle, AYNI_GOAL_SATISFIED);

    // Registration rejects EPHEMERAL combined with query authority != NONE or subscription.
    Node reg_node;
    reg_node.config().call_capability_capacity = 2;
    reg_node.create();
    ayni_schema_node_v1 bad_text{};
    bad_text.kind = AYNI_VALUE_TEXT;
    bad_text.subtree_end = 1;
    bad_text.max_count = 32;
    const ayni_business_contract_v1 bad_biz{{&bad_text, 1}, {&bad_text, 1}, 0, 0, nullptr};
    ayni_call_capability_v1 bad_query{
        sizeof(bad_query),
        1,
        text("bad.query"),
        {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0, AYNI_CALL_IDEMPOTENT,
         AYNI_CALL_QUERY_AUTHORITATIVE, AYNI_CALL_CANCEL_NONE, 0, 5000, AYNI_CALL_INPUT_VALUE,
         AYNI_CALL_RESULT_TEXT, 32, 32, 0, nullptr, nullptr, AYNI_CALL_DURABILITY_EPHEMERAL, 0},
        &bad_biz};
    EXPECT_EQ(ayni_register_call_capability(reg_node.kernel(), &bad_query), AYNI_INVALID_ARGUMENT);
}

// ADR-0110 / C-COMMIT-006: an unrelated pending append must not block the first
// motor's stop proof or the next RAM-only decision, dispatch or receipt.
TEST(EphemeralExecution, motor_actions_handoff_while_journal_is_full) {
    EphemeralFixture fix;
    fix.single_batch_journal = true;
    fix.motor_exec.auto_complete = false;
    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        for (const auto *key : {"first", "second"}) {
            ayni_task_call_result_v1 result{};
            result.struct_size = sizeof(result);
            if (snap.call_result(snap.call_context, text(key), &result) != AYNI_OK) {
                return EphemeralFixture::stage_motor_call(key, 40, out);
            }
            EXPECT_EQ(result.outcome, AYNI_CALL_COMPLETED);
        }
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };
    fix.start();
    const auto goal = fix.create_goal("ephemeral_goal");
    for (unsigned tries = 0; tries < 12 && fix.motor_exec.submit_calls == 0U; ++tries) {
        fix.pump(1);
    }
    ASSERT_EQ(fix.motor_exec.submit_calls, 1U);

    const auto unrelated = WorkNode::create_body(fix.work.instance(agent_a), 0, "resume_goal", 1,
                                                 {}, AYNI_TRIGGER_NONE);
    ASSERT_EQ(fix.work.create(unrelated).status, AYNI_OK);
    const auto prefix = fix.work.node().info().durable_seq;
    ASSERT_EQ(fix.work.create(unrelated).status, AYNI_BUSY);

    fix.motor_exec.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "moved");
    for (unsigned tries = 0; tries < 3 && fix.motor_exec.submit_calls < 2U; ++tries) {
        const auto step = fix.work.node().step();
        if (fix.motor_exec.submit_calls < 2U) {
            EXPECT_EQ(step.more_ready, 1U);
        }
    }
    ASSERT_EQ(fix.motor_exec.submit_calls, 2U);
    EXPECT_EQ(fix.motor_exec.challenge_calls, 2U);
    EXPECT_EQ(fix.motor_exec.stop_calls, 0U);
    EXPECT_EQ(fix.motor_exec.status_calls, 0U);
    fix.motor_exec.finish_active(AYNI_CALL_COMPLETED, AYNI_OK, AYNI_CALL_EFFECT_COMPLETE, "moved");
    fix.work.node().step();
    EXPECT_EQ(fix.work.node().info().durable_seq, prefix);
    EXPECT_EQ(fix.work.goal(goal).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(fix.work.node().step().more_ready, 0U);

    fix.pump(6);
    EXPECT_EQ(fix.work.goal(goal).lifecycle, AYNI_GOAL_SATISFIED);
}

// Test 3 (ADR-0110):
// Multi-action EPHEMERAL turn (model -> motor 1 -> motor 2 -> completion) runs with
// ZERO intermediate WAL appends, preserves volatile steps/checkpoint, allows immediate
// ayni_copy_call_result(), and still enforces max_turns on EPHEMERAL model calls.
TEST(EphemeralExecution, EphemeralMultiActionTurnHasZeroIntermediateWalAppends) {
    EphemeralFixture fix;
    fix.max_turns = 2;
    uint32_t phase = 0;
    size_t log_after_task_create = 0;
    uint64_t durable_after_task_create = 0;
    size_t log_during_actions_max = 0;
    uint64_t model_call_id = 0;
    std::string observed_cp;
    std::vector<std::string> observed_steps;
    bool checked_immediate_result = false;

    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        const auto seen = seen_of(snap);
        if (phase == 0) {
            log_after_task_create = fix.work.node().sim().log().size();
            durable_after_task_create = fix.work.node().info().durable_seq;
            const char *cp = "cp-0";
            std::memcpy(out.checkpoint, cp, 4);
            out.checkpoint_size = 4;
            ++phase;
            return EphemeralFixture::stage_text_call(AYNI_POLICY_REQUEST_MODEL, "m0", "model.bind",
                                                     "plan", out);
        }
        log_during_actions_max =
            std::max(log_during_actions_max, fix.work.node().sim().log().size());
        if (phase == 1) {
            ayni_task_call_result_v1 res{};
            res.struct_size = sizeof(res);
            EXPECT_EQ(snap.call_result(snap.call_context, text("m0"), &res), AYNI_OK);
            EXPECT_EQ(res.outcome, AYNI_CALL_COMPLETED);
            model_call_id = res.logical_call_id;
            EXPECT_EQ(std::string(seen.checkpoint.begin(), seen.checkpoint.end()), "cp-0");
            const char *cp = "cp-1";
            std::memcpy(out.checkpoint, cp, 4);
            out.checkpoint_size = 4;
            ++phase;
            return EphemeralFixture::stage_motor_call("a1", 40, out);
        }
        if (phase == 2) {
            EXPECT_EQ(std::string(seen.checkpoint.begin(), seen.checkpoint.end()), "cp-1");
            const char *cp = "cp-2";
            std::memcpy(out.checkpoint, cp, 4);
            out.checkpoint_size = 4;
            ++phase;
            return EphemeralFixture::stage_motor_call("a2", -40, out);
        }
        observed_cp.assign(seen.checkpoint.begin(), seen.checkpoint.end());
        observed_steps = seen.steps;
        const char *cp = "final!";
        std::memcpy(out.checkpoint, cp, 6);
        out.checkpoint_size = 6;
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };

    fix.start();
    // Hold model_provider on phase 0 so task creation becomes durable before we
    // cut off storage completions for the entire multi-action ephemeral loop.
    fix.model_provider.auto_complete = false;
    const uint64_t goal_id = fix.create_goal("ephemeral_goal");
    fix.pump(3, true);
    const uint64_t task_id = fix.work.goal(goal_id).last_task_id;
    ASSERT_NE(task_id, 0U);
    ASSERT_EQ(fix.model_provider.submit_calls, 1U);
    log_after_task_create = fix.work.node().sim().log().size();
    durable_after_task_create = fix.work.node().info().durable_seq;
    log_during_actions_max = log_after_task_create;

    // Release m0 and run a1 -> a2 -> REPORT_COMPLETION with ZERO storage completions.
    fix.model_provider.complete();
    for (int i = 0; i < 40 && fix.work.goal(goal_id).lifecycle == AYNI_GOAL_ACTIVE; ++i) {
        fix.work.node().step();
        fix.work.node().sim().advance_us(step_tick_us);
        if (model_call_id != 0U && !checked_immediate_result) {
            char buf[32]{};
            uint32_t out_size = 0;
            EXPECT_EQ(ayni_copy_call_result(fix.work.node().kernel(), model_call_id, buf,
                                            sizeof(buf), &out_size),
                      AYNI_OK);
            EXPECT_EQ(std::string(buf, out_size), "ok");
            checked_immediate_result = true;
        }
    }
    // Zero intermediate WAL appends occurred between task start and final completion!
    EXPECT_EQ(log_during_actions_max, log_after_task_create);
    EXPECT_EQ(fix.work.node().info().durable_seq, durable_after_task_create);
    EXPECT_EQ(fix.motor_exec.submit_calls, 2U);
    EXPECT_EQ(fix.motor_exec.stop_calls, 0U);
    EXPECT_EQ(fix.motor_exec.status_calls, 0U);
    EXPECT_EQ(observed_cp, "cp-2");
    ASSERT_EQ(observed_steps.size(), 3U);
    EXPECT_EQ(observed_steps[0], "m0");
    EXPECT_EQ(observed_steps[1], "a1");
    EXPECT_EQ(observed_steps[2], "a2");
    EXPECT_TRUE(checked_immediate_result);

    // Now complete storage for the single final REPORT_COMPLETION commit and verify checkpoint.
    fix.pump(5, true);
    EXPECT_EQ(fix.work.node().info().durable_seq, durable_after_task_create + 1U);
    EXPECT_EQ(fix.work.node().sim().log().size(), log_after_task_create + 1U);
    EXPECT_EQ(fix.work.goal(goal_id).lifecycle, AYNI_GOAL_SATISFIED);
    const auto final_task = fix.work.task(task_id);
    EXPECT_EQ(final_task.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(final_task.model_calls_used, 1U);
    uint8_t cp_buf[32]{};
    uint32_t cp_size = 0;
    ASSERT_EQ(ayni_copy_task_checkpoint(fix.work.node().kernel(), task_id, cp_buf, sizeof(cp_buf),
                                        &cp_size),
              AYNI_OK);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(cp_buf), cp_size), "final!");

    // Verify max_turns (=2) is strictly enforced on EPHEMERAL model calls and folds cleanly.
    fix.model_provider.auto_complete = true;
    uint32_t model_attempts = 0;
    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &, ayni_policy_output_v1 &out) {
        const std::string key = "turn" + std::to_string(model_attempts++);
        return EphemeralFixture::stage_text_call(AYNI_POLICY_REQUEST_MODEL, key.c_str(),
                                                 "model.bind", "q", out);
    };
    const uint64_t g_limit = fix.create_goal("ephemeral_goal");
    fix.pump(30, true);
    const auto limited_task = fix.work.task(fix.work.goal(g_limit).last_task_id);
    EXPECT_EQ(limited_task.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(limited_task.hold_status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(limited_task.hold_reason, AYNI_REASON_TASK_MODEL_CALL_LIMIT);
    EXPECT_EQ(limited_task.model_calls_used, 2U);
}

// Test 4 (ADR-0110 / C-STOP-006):
// Forged/mismatched/active inline stop proofs and unflagged executors fail closed
// into normal actuator QUARANTINED + STOP + STATUS polling before Action 2 can run.
TEST(EphemeralExecution, ForgedOrUnflaggedInlineStopProofFallsBackToQuarantinedPolling) {
    struct Case {
        uint32_t flags;
        InlineTestExecutor::ProofMode mode;
    };
    const Case cases[] = {
        {0U, InlineTestExecutor::ProofMode::valid_stopped},
        {AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF, InlineTestExecutor::ProofMode::wrong_permit_id},
        {AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF, InlineTestExecutor::ProofMode::still_active},
    };
    for (const auto &tc : cases) {
        EphemeralFixture fix;
        fix.motor_exec.flags = tc.flags;
        fix.motor_exec.proof_mode = tc.mode;
        uint32_t step_num = 0;
        fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                         ayni_policy_output_v1 &out) {
            if (step_num == 0) {
                ++step_num;
                return EphemeralFixture::stage_motor_call("a1", 25, out);
            }
            if (step_num == 1) {
                ++step_num;
                return EphemeralFixture::stage_motor_call("a2", -25, out);
            }
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        };
        fix.start();
        const uint64_t goal_id = fix.create_goal("ephemeral_goal");
        fix.pump(40, true);
        EXPECT_EQ(fix.work.goal(goal_id).lifecycle, AYNI_GOAL_SATISFIED);
        EXPECT_EQ(fix.motor_exec.submit_calls, 2U);
        // Fail-closed fallback MUST have invoked STOP + STATUS on both actions!
        EXPECT_GE(fix.motor_exec.stop_calls, 2U);
        EXPECT_GE(fix.motor_exec.status_calls, 2U);
    }
}

// Test 5 (ADR-0110):
// After power cut / reboot, unfinished CANCEL_ON_REBOOT Goals and Tasks automatically
// cancel in Boot 2 with ZERO strategy decide() replays, while sibling RESUME Goals continue.
TEST(EphemeralExecution, CancelOnRebootGoalAutoCancelsAcrossRebootWithZeroReplay) {
    ayni::hosts::sim::StorageImage saved_storage;
    uint64_t ephem_goal_id = 0;
    uint64_t ephem_task_id = 0;
    uint64_t resume_goal_id = 0;
    {
        EphemeralFixture boot1;
        boot1.motor_exec.auto_complete = false;
        uint32_t ephem_decides = 0;
        uint32_t resume_steps = 0;
        boot1.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                           ayni_policy_output_v1 &out) {
            if (string_of(snap.goal_type_id) == "ephemeral_goal") {
                if (ephem_decides++ == 0) {
                    return EphemeralFixture::stage_motor_call("a1", 50, out);
                }
                return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
            }
            // Keep resume_goal actively progressing in Boot 1 so it is still live at power cut.
            if (resume_steps < 8U) {
                const std::string key = "r" + std::to_string(resume_steps++);
                out.intents[0] = progress_intent(key, snap.revision);
                out.intent_count = 1;
                return AYNI_OK;
            }
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        };
        boot1.start();
        ephem_goal_id = boot1.create_goal("ephemeral_goal");
        resume_goal_id = boot1.create_goal("resume_goal");
        // Advance just enough for both goals/tasks to be persisted and motor 1 to be in flight.
        boot1.pump(4, true);
        ephem_task_id = boot1.work.goal(ephem_goal_id).last_task_id;
        ASSERT_NE(ephem_task_id, 0U);
        ASSERT_EQ(boot1.motor_exec.submit_calls, 1U);
        ASSERT_EQ(boot1.work.goal(resume_goal_id).lifecycle, AYNI_GOAL_ACTIVE);
        // Simulate abrupt power cut while motor 1 is still in flight.
        saved_storage = boot1.work.node().sim().durable_storage();
        // Cleanly stop active motor before boot1 destructor drains.
        boot1.motor_exec.finish_active(AYNI_CALL_CANCELLED, AYNI_OK, AYNI_CALL_EFFECT_PARTIAL,
                                       "cut");
    }

    // Boot 2 recovers from saved_storage (boot_epoch = 2).
    EphemeralFixture boot2;
    uint32_t ephem_replays = 0;
    uint32_t resume_replays = 0;
    boot2.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                       ayni_policy_output_v1 &out) {
        if (string_of(snap.goal_type_id) == "ephemeral_goal") {
            ++ephem_replays;
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        }
        ++resume_replays;
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };
    boot2.configure_node();
    boot2.work.node().sim().durable_storage() = saved_storage;
    boot2.work.node().create_and_start();
    boot2.work.node().recover();
    boot2.pump(30, true);

    EXPECT_EQ(ephem_replays, 0U);
    EXPECT_GE(resume_replays, 1U);
    EXPECT_EQ(boot2.work.goal(ephem_goal_id).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(boot2.work.task(ephem_task_id).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(boot2.work.goal(resume_goal_id).lifecycle, AYNI_GOAL_SATISFIED);
}

// Test 6 (ADR-0110 / C-AUTH-026 / C-STOP-001):
// In-flight EPHEMERAL physical calls immediately trigger urgent STOP upon either
// on-demand Grant revocation or Scene disable.
TEST(EphemeralExecution, EphemeralPhysicalCallObeysLiveGrantRevocationAndSceneDisable) {
    for (int subcase = 0; subcase < 2; ++subcase) {
        EphemeralFixture fix;
        fix.on_demand_tools = (subcase == 0);
        fix.motor_exec.auto_complete = false;
        uint32_t decides = 0;
        fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                         ayni_policy_output_v1 &out) {
            if (decides++ == 0) {
                return EphemeralFixture::stage_motor_call("a1", 50, out);
            }
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        };
        fix.start();
        const uint64_t goal_id = fix.create_goal("ephemeral_goal");
        fix.pump(12, true);
        ASSERT_EQ(fix.motor_exec.submit_calls, 1U);
        ASSERT_EQ(fix.motor_exec.stop_calls, 0U);

        if (subcase == 0) {
            // Revoke the on-demand Grant while the EPHEMERAL physical call is running.
            const ayni_grant_revoke_v1 target{sizeof(target), 0, fix.motor_grant_id};
            auto intent = fix.work.intent(AYNI_INTENT_GRANT_REVOKE, &target, 1);
            intent.command_id = ++fix.next_command;
            ASSERT_EQ(fix.work.submit(intent).status, AYNI_OK);
        } else {
            // Disable the Scene while the EPHEMERAL physical call is running.
            fix.set_scene(false);
        }
        fix.pump(15, true);
        // Independent physical stop port MUST be invoked immediately on authority/scene loss.
        EXPECT_GE(fix.motor_exec.stop_calls, 1U);
        // Complete the cancelled motor attempt and verify clean drain.
        fix.motor_exec.finish_active(AYNI_CALL_CANCELLED, AYNI_OK, AYNI_CALL_EFFECT_PARTIAL,
                                     "stopped");
        fix.pump(15, true);
        EXPECT_NE(fix.work.goal(goal_id).lifecycle, AYNI_GOAL_PROPOSED);
    }
}

// Test 7 (ADR-0110):
// Mixed Tier 0 (EPHEMERAL) + Tier 2 (DURABLE) + Tier 0 (EPHEMERAL) within one
// CANCEL_ON_REBOOT Task keeps Tier 2 durable barriers intact, skips CALL WAL on Tier 0,
// and replays cleanly after restart.
TEST(EphemeralExecution, MixedEphemeralAndDurableCallsPreserveDurabilityAndReplay) {
    ayni::hosts::sim::StorageImage saved_storage;
    uint64_t goal_id = 0;
    uint64_t task_id = 0;
    {
        EphemeralFixture fix;
        uint32_t stage = 0;
        fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                         ayni_policy_output_v1 &out) {
            if (stage == 0) {
                ++stage;
                const char *cp = "s0";
                std::memcpy(out.checkpoint, cp, 2);
                out.checkpoint_size = 2;
                return EphemeralFixture::stage_motor_call("e1", 20, out);
            }
            if (stage == 1) {
                ++stage;
                const char *cp = "s1";
                std::memcpy(out.checkpoint, cp, 2);
                out.checkpoint_size = 2;
                return EphemeralFixture::stage_text_call(AYNI_POLICY_REQUEST_CALL, "d2",
                                                         "durable.bind", "persist", out);
            }
            if (stage == 2) {
                ++stage;
                const char *cp = "s2";
                std::memcpy(out.checkpoint, cp, 2);
                out.checkpoint_size = 2;
                return EphemeralFixture::stage_motor_call("e3", -20, out);
            }
            const char *cp = "done";
            std::memcpy(out.checkpoint, cp, 4);
            out.checkpoint_size = 4;
            return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
        };
        fix.start();
        goal_id = fix.create_goal("ephemeral_goal");
        fix.pump(60, true);
        task_id = fix.work.goal(goal_id).last_task_id;
        EXPECT_EQ(fix.work.goal(goal_id).lifecycle, AYNI_GOAL_SATISFIED);
        EXPECT_EQ(fix.work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
        EXPECT_EQ(fix.motor_exec.submit_calls, 2U);
        EXPECT_EQ(fix.durable_provider.submit_calls, 1U);
        EXPECT_EQ(fix.motor_exec.stop_calls, 0U);
        saved_storage = fix.work.node().sim().durable_storage();
    }

    // Recover from the recorded WAL to prove current-schema replay consistency.
    EphemeralFixture replay;
    replay.configure_node();
    replay.work.node().sim().durable_storage() = saved_storage;
    replay.work.node().create_and_start();
    replay.work.node().recover();
    replay.pump(15, true);
    EXPECT_EQ(replay.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(replay.work.goal(goal_id).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(replay.work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
    uint8_t cp[16]{};
    uint32_t cp_size = 0;
    ASSERT_EQ(
        ayni_copy_task_checkpoint(replay.work.node().kernel(), task_id, cp, sizeof(cp), &cp_size),
        AYNI_OK);
    EXPECT_EQ(std::string(reinterpret_cast<char *>(cp), cp_size), "done");
}

// Test 8 (ADR-0110): FENCED reload must not deadlock when an active EPHEMERAL physical Call has
// no inline stop proof (or finishes ABANDONED) and relies on async AYNI_EXECUTOR_STATUS stop proof.
TEST(EphemeralExecution, FencedReloadCompletesForEphemeralPhysicalCallWithoutInlineStopProof) {
    EphemeralFixture fix;
    fix.motor_exec.flags = 0; // Disable inline stop proof so stop uses async STOP + STATUS.
    fix.motor_exec.proof_mode = InlineTestExecutor::ProofMode::omit;
    fix.motor_exec.auto_complete = false;
    bool dispatched = false;
    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        if (!dispatched) {
            dispatched = true;
            return EphemeralFixture::stage_motor_call("e_fenced", 25, out);
        }
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };
    fix.start();
    const uint64_t motor_goal = fix.create_goal("ephemeral_goal");
    ASSERT_NE(motor_goal, 0U);
    for (int i = 0; i < 30 && !fix.motor_exec.active.has_value(); ++i) {
        fix.pump(1, true);
    }
    ASSERT_TRUE(fix.motor_exec.active.has_value());

    // Inject transient append faults on an unrelated durable goal to push Kernel into FENCED.
    using ayni::hosts::sim::StorageFault;
    using ayni::hosts::sim::StoragePort;
    for (int attempt = 0; attempt < 3; ++attempt) {
        fix.work.node().sim().inject_fault(StoragePort::append, StorageFault::unavailable);
    }
    static_cast<void>(fix.create_goal("resume_goal"));
    for (int i = 0; i < 40 && fix.work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING; ++i) {
        fix.work.node().sim().advance_us(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
        fix.work.node().sim().complete_storage();
        fix.work.node().step(1);
    }
    ASSERT_EQ(fix.work.node().lifecycle(), AYNI_LIFECYCLE_FENCED);

    // Deliver ABANDONED terminal without inline stop proof; ExecutionControl stops via STATUS,
    // releases the ephemeral lease, exits FENCED into RECOVERING, and reloads cleanly to RUNNING.
    fix.motor_exec.finish_active(AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_CALL_EFFECT_UNKNOWN,
                                 nullptr);
    for (int i = 0; i < 80 && fix.work.node().lifecycle() != AYNI_LIFECYCLE_RUNNING; ++i) {
        fix.work.node().sim().advance_us(poll_period_us);
        fix.work.node().sim().complete_storage();
        fix.work.node().step();
    }
    EXPECT_EQ(fix.work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_GT(fix.motor_exec.stop_calls, 0U);
}

} // namespace

namespace {
// C-CALL-016/ADR-0115: Tier0 UNKNOWN holds while live, but does not permanently pin stopped work.
TEST(EphemeralExecution, StoppedUnknownHoldsUntilCancellationThenRetiresWithoutLedgerOrCallWal) {
    EphemeralFixture fix;
    fix.work.node().config().task_capacity = 1;
    fix.work.node().config().audit_capacity = 64;
    fix.work.node().config().unresolved_op_capacity = 1;
    fix.motor_exec.auto_complete = false;
    fix.motor_exec.proof_mode = InlineTestExecutor::ProofMode::omit;
    bool dispatched = false;
    fix.work.strategy().answer = [&](const ayni_task_snapshot_v1 &snap,
                                     ayni_policy_output_v1 &out) {
        if (!dispatched) {
            dispatched = true;
            return EphemeralFixture::stage_motor_call("unknown", 20, out);
        }
        return report(AYNI_POLICY_REPORT_COMPLETION, snap, out);
    };
    fix.start();
    const auto goal = fix.create_goal("ephemeral_goal");
    for (uint32_t at = 0; at < 30 && !fix.motor_exec.active; ++at) {
        fix.pump(1);
    }
    ASSERT_TRUE(fix.motor_exec.active);
    const auto task = fix.work.goal(goal).last_task_id;
    const auto call = fix.work.task(task).logical_call_id;
    auto &model = ayni::kernel::KernelTestProbe::work(fix.work.kernel());
    // White-box: in-flight Provider borrow and unconfirmed physical stop both forbid retirement.
    EXPECT_FALSE(model.calls.can_retire_task(task, UINT64_MAX));
    fix.motor_exec.finish_active(AYNI_CALL_ABANDONED, AYNI_RESULT_UNKNOWN, AYNI_CALL_EFFECT_UNKNOWN,
                                 nullptr);
    fix.pump(30);
    EXPECT_EQ(fix.work.task(task).hold_status, AYNI_RESULT_UNKNOWN);
    EXPECT_EQ(fix.motor_exec.submit_calls, 1U);
    EXPECT_EQ(model.unresolved_ops.size(), 0U);
    ASSERT_EQ(fix.work.cancel_task(task).status, AYNI_OK);
    fix.pump(8);
    fix.create_goal("ephemeral_goal");
    fix.pump(16);
    ayni_task_info_v1 retired{};
    retired.struct_size = sizeof(retired);
    EXPECT_EQ(ayni_get_task(fix.work.kernel(), task, &retired), AYNI_NOT_FOUND);
    ayni_call_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_call(fix.work.kernel(), call, &info), AYNI_NOT_FOUND);
    EXPECT_EQ(model.unresolved_ops.size(), 0U);
    std::array<ayni_audit_record_v1, 64> records{};
    uint32_t count = 0;
    ASSERT_EQ(ayni_take_audit_records(fix.work.kernel(), records.data(), records.size(), &count),
              AYNI_OK);
    bool found = false;
    for (uint32_t at = 0; at < count; ++at) {
        if (records[at].kind == AYNI_AUDIT_CALL_TERMINAL && records[at].subject_id == call) {
            found = true;
            EXPECT_NE(records[at].flags & AYNI_AUDIT_FLAG_EPHEMERAL, 0U);
        }
    }
    EXPECT_TRUE(found);
    for (const auto &stored : fix.work.node().sim().log()) {
        for (const auto &record : read_stored_batch(stored).records) {
            EXPECT_NE(record.header.type, ayni::work::call_record_type);
        }
    }
}
} // namespace
