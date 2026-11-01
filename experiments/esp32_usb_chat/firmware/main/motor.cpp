#include "motor.h"
#include "motor_contract.h"
#include <ayni/directory.h>
#include <ayni/resources.h>
#include <ayni/scenes.h>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
namespace usbchat {
namespace {
constexpr uint32_t version = 1, reply_bytes_max = 1024, input_bytes_max = 256;
constexpr uint32_t motor1_field = 1, motor2_field = 2, duration_field = 3;
constexpr uint32_t request_timeout_ms = 1000;
constexpr const char *identities[] = {"motor.status", "motor.plan", "motor.run", "motor.stop"};
ayni_string_v1 text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
bool same(ayni_string_v1 a, const char *b) {
    return a.size == std::strlen(b) && !std::memcmp(a.data, b, a.size);
}
} // namespace
ayni_status_t Motor::configure(ayni_kernel_t *kernel) {
    auto checked = [&](const char *stage, ayni_status_t rc) {
        if (rc != AYNI_OK) {
            char message[128];
            const auto count = std::snprintf(message, sizeof(message), "motor registration %s=%ld",
                                             stage, (long)rc);
            if (count > 0 && size_t(count) < sizeof(message)) {
                transport_.send(Kind::status, 0, message, size_t(count));
            }
        }
        return rc;
    };
    auto initialized = execution_.initialize();
    if (initialized != AYNI_OK) {
        return initialized;
    }
    status_input_.kind = AYNI_VALUE_OBJECT;
    status_input_.subtree_end = 1;
    result_.kind = AYNI_VALUE_TEXT;
    result_.subtree_end = 1;
    result_.max_count = reply_bytes_max;
    plan_input_[0].kind = AYNI_VALUE_OBJECT;
    plan_input_[0].subtree_end = plan_input_.size();
    for (uint32_t at = motor1_field; at < plan_input_.size(); ++at) {
        auto &node = plan_input_[at];
        node.kind = at == duration_field ? AYNI_VALUE_U64 : AYNI_VALUE_I64;
        node.field_id = at;
        node.subtree_end = at + 1;
        node.flags = AYNI_SCHEMA_REQUIRED;
        node.name = text(at == motor1_field   ? "motor1_percent"
                         : at == motor2_field ? "motor2_percent"
                                              : "duration_ms");
        node.unit = text(at == duration_field ? "ms" : "percent");
        if (at == duration_field) {
            node.min_u64 = 1;
            node.max_u64 = motor_plan_duration_ms_max;
        } else {
            node.min_i64 = -motor_percent_max;
            node.max_i64 = motor_percent_max;
        }
    }
    business_[0] = {{&status_input_, 1}, {&result_, 1}, 0, 0, nullptr};
    business_[1] = {
        {plan_input_.data(), uint32_t(plan_input_.size())}, {&result_, 1}, 0, 0, nullptr};
    run_input_ = plan_input_;
    const auto limits = motor_limits();
    std::array<ayni_authority_field_desc_v1, 3> fields{};
    for (uint32_t at = 0; at < fields.size(); ++at) {
        auto &node = run_input_[at + 1];
        node.kind = AYNI_VALUE_I64;
        node.min_u64 = node.max_u64 = 0;
        node.min_i64 = limits.fields[at].minimum;
        node.max_i64 = limits.fields[at].maximum;
        auto &field = fields[at];
        field.struct_size = sizeof(field);
        field.field_id = at + 1;
        field.usage = AYNI_AUTHORITY_PARAMETER;
        field.domain.struct_size = sizeof(field.domain);
        field.domain.kind = AYNI_AUTHORITY_I64;
        field.domain.unit = node.unit;
        field.domain.values.integer = {node.min_i64, node.max_i64};
        projections_[at].field_id = at + 1;
        projections_[at].path_count = 1;
        projections_[at].path[0] = at + 1;
        projections_[at].unit = node.unit;
    }
    business_[2] = {{run_input_.data(), uint32_t(run_input_.size())},
                    {&result_, 1},
                    uint32_t(projections_.size()),
                    0,
                    projections_.data()};
    business_[3] = business_[0];
    resource_ = {text("motor.actuator"), 1, 0};
    for (uint32_t at = 0; at < offers_.size(); ++at) {
        const ayni_capability_desc_v1 authority{
            sizeof(authority), version, text(identities[at]),
            at == 2 ? 3U : 0U, 0,       at == 2 ? fields.data() : nullptr};
        auto rc = checked(identities[at], ayni_register_capability(kernel, &authority));
        if (rc != AYNI_OK) {
            return rc;
        }
        offers_[at] = {sizeof(ayni_call_capability_v1),
                       version,
                       text(identities[at]),
                       {sizeof(ayni_call_contract_v1), AYNI_CALL_UNARY, AYNI_CALL_TOOL, 0,
                        AYNI_CALL_IDEMPOTENT, AYNI_CALL_QUERY_NONE, AYNI_CALL_CANCEL_NONE, 0,
                        request_timeout_ms, AYNI_CALL_INPUT_VALUE, AYNI_CALL_RESULT_TEXT,
                        input_bytes_max, reply_bytes_max, 0, nullptr, nullptr},
                       &business_[at]};
        if (at == 2) {
            auto &contract = offers_[at].contract;
            contract.form = AYNI_CALL_ACTION;
            contract.effects = AYNI_CALL_EFFECT_PHYSICAL;
            contract.idempotency = AYNI_CALL_NOT_IDEMPOTENT;
            contract.cancel = AYNI_CALL_CANCEL_REQUEST;
            contract.timeout_ms = 90000;
            contract.resource_count = 1;
            contract.resources = &resource_;
        }
        rc = checked("call schema", ayni_register_call_capability(kernel, &offers_[at]));
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    provider_ = {sizeof(provider_),
                 version,
                 text("motor.local"),
                 text("motor.ta6586"),
                 {},
                 this,
                 uint32_t(offers_.size()),
                 0,
                 offers_.data(),
                 submit,
                 cancel,
                 nullptr,
                 nullptr};
    auto rc = checked("provider", ayni_register_provider(kernel, &provider_));
    for (uint32_t at = 0; rc == AYNI_OK && at < offers_.size(); ++at) {
        const ayni_binding_v1 binding{sizeof(binding),
                                      version,
                                      text(identities[at]),
                                      text(identities[at]),
                                      version,
                                      version,
                                      provider_.provider_id,
                                      provider_.endpoint_id,
                                      {},
                                      0,
                                      0,
                                      nullptr};
        rc = checked("binding", ayni_register_binding(kernel, &binding));
        if (rc != AYNI_OK) {
            break;
        }
        constexpr const char *summaries[]{
            "Read TA6586 wiring and manual-bench readiness; power/shaft state is not measured.",
            "Compute a TA6586 software plan without output.",
            "Run a bounded TA6586 PWM procedure only after per-action Mac human approval.",
            "Request latched local motor output stop. Reconnection with driver power off is "
            "required to rearm."};
        constexpr const char *parameters[]{
            "Empty object.",
            "Required integer motor1_percent and motor2_percent: -90..90; duration_ms: 1..5000.",
            "Required integers motor1_percent and motor2_percent: -90..90; duration_ms: 50..5000, "
            "multiple of 5. "
            "Zero coasts a motor; both zero is invalid (use motor.stop). Positive D0/D2 PWM, "
            "negative D1/D3 PWM. "
            "This is PWM duty, not RPM; shaft direction is uncalibrated. Runtime operator cap may "
            "be lower than 90. "
            "Every invocation requires separate Mac approval; do not retry declined, expired or "
            "unknown actions.",
            "Empty object. This tool is not an independent hardware stop; the Mac stop button "
            "bypasses model execution."};
        const ayni_tool_field_usage_v1 descriptions[]{
            {sizeof(ayni_tool_field_usage_v1), motor1_field,
             text("Motor 1 signed PWM percent; zero coasts, no RPM measurement.")},
            {sizeof(ayni_tool_field_usage_v1), motor2_field,
             text("Motor 2 signed PWM percent; zero coasts, no RPM measurement.")},
            {sizeof(ayni_tool_field_usage_v1), duration_field,
             text("Output duration in ms; multiple of 5, at most 5000.")}};
        const ayni_tool_usage_v1 usage{
            sizeof(usage),
            version,
            binding.binding_id,
            version,
            binding.capability_id,
            text(summaries[at]),
            text(parameters[at]),
            text(at == 2
                     ? "Reports actual waveform and locally measured controller output-stop facts. "
                       "No wheel observation is inferred. "
                       "Target, wheel distance and RPM remain unverified. Manual raised-wheel "
                       "bench only; "
                       "external watchdog/reset-disable are not verified."
                     : "Configured wiring/supply are not electrical measurements. A plan does "
                       "not move motors."),
            text(at == 1 || at == 2
                     ? "{\"motor1_percent\":50,\"motor2_percent\":0,\"duration_ms\":5000}"
                     : "{}"),
            at == 2 ? 3U : 0U,
            0,
            at == 2 ? descriptions : nullptr};
        rc = checked("usage", ayni_register_capability_usage(kernel, text(summaries[at]), &usage));
    }
    const ayni_resource_desc_v1 resource{sizeof(resource),
                                         AYNI_RESOURCE_EXCLUSIVE,
                                         resource_.resource_id,
                                         1,
                                         AYNI_RESOURCE_ACTUATOR,
                                         0,
                                         0,
                                         nullptr,
                                         {}};
    if (rc == AYNI_OK) {
        rc = checked("resource", ayni_register_resource(kernel, &resource));
    }
    if (rc == AYNI_OK) {
        rc = checked("health",
                     ayni_set_resource_health(kernel, resource.resource_id, AYNI_RESOURCE_ONLINE));
    }
    const auto ports = execution_.ports();
    if (rc == AYNI_OK) {
        rc = checked("executor", ayni_register_executor(kernel, &ports));
    }
    const ayni_scene_rule_v1 rule{
        text("motor.run"), version, 0, {sizeof(ayni_scope_v1), 0, nullptr}};
    const ayni_scene_desc_v1 scene{sizeof(scene), version, text(motor_scene), 1, 0, &rule, nullptr};
    if (rc == AYNI_OK) {
        rc = checked("scene", ayni_register_scene(kernel, &scene));
    }
    std::array<ayni_execution_field_v1, 3> mappings{};
    // ExecutionBinding channels are lexically sorted; authority Schema IDs stay 1/2/3.
    constexpr uint32_t channel_order[]{2, 0, 1};
    for (uint32_t at = 0; at < mappings.size(); ++at) {
        const auto channel = channel_order[at];
        mappings[at] = {text(motor_fields[channel]), text(channel == 2 ? "ms" : "percent"),
                        channel + 1, channel + 1};
    }
    const ayni_execution_binding_v1 execution{sizeof(execution),
                                              version,
                                              text("motor.run"),
                                              ports.executor_id,
                                              version,
                                              version,
                                              text(motor_scene),
                                              AYNI_FORM_PROCEDURE,
                                              motor_procedure_us,
                                              motor_sample_age_us,
                                              uint32_t(mappings.size()),
                                              mappings.data()};
    if (rc == AYNI_OK) {
        rc = checked("execution", ayni_register_execution_binding(kernel, &execution));
    }
    return rc;
}
ayni_schema_v1 Motor::schema(ayni_string_v1 binding) const {
    for (uint32_t at = 0; at < offers_.size(); ++at) {
        if (same(binding, identities[at])) {
            return business_[at].input;
        }
    }
    return {};
}
ayni_submit_result_t Motor::submit(void *ctx, const ayni_provider_request_v1 *request,
                                   ayni_provider_completion_t *completion) {
    if (!request || !completion) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    auto &self = *static_cast<Motor *>(ctx);
    if (same(request->capability_id, identities[2])) {
        return self.execution_.submit(request, completion);
    }
    if (same(request->capability_id, identities[3])) {
        // Request only: GPIO work happens in the owner loop after returning to Core.
        self.execution_.request_local_stop();
    }
    char reply[reply_bytes_max];
    int size = 0;
    if (same(request->capability_id, identities[0])) {
        size = self.execution_.describe(reply, sizeof(reply));
    } else if (same(request->capability_id, identities[1])) {
        ayni::ValueReader value;
        if (value.read(request->value) != AYNI_OK) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        const auto *first = value.field(motor1_field), *second = value.field(motor2_field);
        const auto *duration = value.field(duration_field);
        if (!first || !second || !duration || first->kind != AYNI_VALUE_I64 ||
            second->kind != AYNI_VALUE_I64 || duration->kind != AYNI_VALUE_U64) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        const auto duty =
            motor_duties(int32_t(first->value.integer), int32_t(second->value.integer));
        size = std::snprintf(
            reply, sizeof(reply),
            "{\"motion_enabled\":false,\"executed\":false,\"duration_ms\":%lu,"
            "\"gpio_d0_d1_d2_d3\":[%lu,%lu,%lu,%lu],\"planned_duty_percent\":[%lu,%lu,%lu,%lu],"
            "\"after_plan\":\"all-low-coast\",\"mode\":\"software-planning-only\"}",
            static_cast<unsigned long>(duration->value.unsigned_integer),
            static_cast<unsigned long>(motor_pins[0]), static_cast<unsigned long>(motor_pins[1]),
            static_cast<unsigned long>(motor_pins[2]), static_cast<unsigned long>(motor_pins[3]),
            static_cast<unsigned long>(duty[0]), static_cast<unsigned long>(duty[1]),
            static_cast<unsigned long>(duty[2]), static_cast<unsigned long>(duty[3]));
    } else if (same(request->capability_id, identities[3])) {
        size = std::snprintf(reply, sizeof(reply),
                             "{\"stop_requested\":true,\"stop_confirmed\":false}");
    } else {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    if (size <= 0 || size_t(size) >= sizeof(reply)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = request->attempt;
    result.outcome = AYNI_CALL_COMPLETED;
    result.status = AYNI_OK;
    result.effect = AYNI_CALL_EFFECT_COMPLETE; // Requested computation completed; no motion effect.
    result.result = {reply, uint32_t(size)};
    return ayni_provider_post(completion, &result) == AYNI_OK ? AYNI_SUBMIT_ACCEPTED
                                                              : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t Motor::cancel(void *ctx, ayni_attempt_ref_v1 attempt) {
    return static_cast<Motor *>(ctx)->execution_.cancel(attempt);
}
} // namespace usbchat
