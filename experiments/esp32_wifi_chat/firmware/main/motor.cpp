#include "motor.h"
#include "motor_contract.h"
#include <ayni/directory.h>
#include <ayni/resources.h>
#include <ayni/scenes.h>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
namespace wifichat {
namespace {
constexpr uint32_t version = motor_version, reply_bytes_max = motor_reply_bytes_max,
                   input_bytes_max = motor_input_bytes_max;
constexpr uint32_t motor1_field = 1, motor2_field = 2, duration_field = 3;
constexpr uint32_t request_timeout_ms = 1000;
constexpr const char *identities[] = {"motor.status", "motor.run", "motor.stop"};
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
    // Register contracts at boot; hardware and the running worker are initialized automatically
    // after registration. The composition reserves the control stack before Host startup.
    status_input_.kind = AYNI_VALUE_OBJECT;
    status_input_.subtree_end = 1;
    result_.kind = AYNI_VALUE_TEXT;
    result_.subtree_end = 1;
    result_.max_count = reply_bytes_max;
    business_[0] = {{&status_input_, 1}, {&result_, 1}, 0, 0, nullptr};
    run_input_ = motor_run_schema();
    std::array<ayni_authority_field_desc_v1, 3> fields{};
    for (uint32_t at = 0; at < fields.size(); ++at) {
        auto &node = run_input_[at + 3];
        auto &field = fields[at];
        field.struct_size = sizeof(field);
        field.field_id = at + 1;
        field.usage = AYNI_AUTHORITY_PARAMETER;
        field.domain.struct_size = sizeof(field.domain);
        field.domain.kind = AYNI_AUTHORITY_I64;
        field.domain.unit = node.unit;
        field.domain.values.integer = {node.min_i64, node.max_i64};
        projections_[at].field_id = at + 1;
        projections_[at].path_count = 3;
        projections_[at].path[0] = 1; // Root field "segments" list.
        projections_[at].path[1] = AYNI_PROJECTION_LIST_ITEM;
        projections_[at].path[2] = at + 1;
        projections_[at].unit = node.unit;
    }
    business_[1] = {{run_input_.data(), uint32_t(run_input_.size())},
                    {&result_, 1},
                    uint32_t(projections_.size()),
                    0,
                    projections_.data()};
    business_[2] = business_[0];
    resource_ = {text("motor.actuator"), 1, 0};
    for (uint32_t at = 0; at < offers_.size(); ++at) {
        const ayni_capability_desc_v1 authority{
            sizeof(authority), version, text(identities[at]),
            at == 1 ? 3U : 0U, 0,       at == 1 ? fields.data() : nullptr};
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
                        input_bytes_max, reply_bytes_max, 0, nullptr, nullptr,
                        AYNI_CALL_DURABILITY_EPHEMERAL, 0},
                       &business_[at]};
        if (at == 1) {
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
            "Run a bounded 1..8 segment TA6586 PWM session continuously while local motor control "
            "is enabled; no per-action approval.",
            "Request recoverable local motor soft-stop and confirm GPIO low readback; stays "
            "armed unless stop fails."};
        constexpr const char *parameters[]{
            "UNARY: use operation=\"invoke\" or omit operation; start is unsupported. Empty "
            "object.",
            "ACTION: operation=\"invoke\" waits for completion (managed start is unsupported on "
            "ephemeral bench motor calls). "
            "Required field segments: ordered array of 1..8 objects, each with required integers "
            "motor1_percent and motor2_percent (-90..90) and duration_ms (50..60000, multiple of "
            "5). Total duration across all segments must not exceed 60000 ms; at least one "
            "segment must be nonzero (a (0,0) segment coasts for a pause between motions). "
            "Segments switch seamlessly in one ControlSession without stopping between them. "
            "Do not insert motor.stop for a pause between motions unless stopping is requested; "
            "motor.stop performs a recoverable soft-stop (stays armed after confirmed stop). "
            "Motor 1 positive D0 PWM, negative D1 PWM; motor 2 positive D3 PWM, negative D2 PWM "
            "(board polarity corrected). This is PWM duty, not RPM; actual shaft direction is not "
            "measured. Runtime operator cap may be lower than 90. Resources initialize "
            "automatically; control defaults enabled after session readiness.",
            "UNARY: use operation=\"invoke\" or omit operation; start is unsupported. Empty "
            "object. Performs a recoverable soft-stop with GPIO low readback; "
            "this tool is not an independent hardware stop — the touchscreen red stop button "
            "bypasses model execution and latches until reboot."};
        const ayni_tool_field_usage_v1 descriptions[]{
            {sizeof(ayni_tool_field_usage_v1), motor1_field,
             text("Motor 1 signed PWM percent per segment; zero coasts, no RPM measurement.")},
            {sizeof(ayni_tool_field_usage_v1), motor2_field,
             text("Motor 2 signed PWM percent per segment; zero coasts, no RPM measurement.")},
            {sizeof(ayni_tool_field_usage_v1), duration_field,
             text("Segment PWM duration in ms; multiple of 5, total across segments <= 60000.")}};
        const ayni_tool_usage_v1 usage{
            sizeof(usage),
            version,
            binding.binding_id,
            version,
            binding.capability_id,
            text(summaries[at]),
            text(parameters[at]),
            text(at == 1 ? "Reports actual multi-segment waveform and locally measured controller "
                           "output-stop facts. No wheel observation is inferred. "
                           "Target, wheel distance and RPM remain unverified. Manual raised-wheel "
                           "bench only; external watchdog/reset-disable are not verified."
                 : at == 2
                     ? "Confirms controller PWM disable and GPIO low readback (stop_confirmed) "
                       "and whether motor stays armed; does not measure shaft standstill or "
                       "driver power-off. Query motor.status for local controller state."
                     : "Configured wiring/supply are not electrical measurements."),
            text(at == 1 ? "{\"segments\":[{\"motor1_percent\":50,\"motor2_percent\":50,\"duration_"
                           "ms\":1000}]}"
                         : "{}"),
            at == 1 ? 3U : 0U,
            0,
            at == 1 ? descriptions : nullptr};
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
                                              AYNI_FORM_SESSION,
                                              motor_session_lease_us,
                                              motor_sample_age_us,
                                              uint32_t(mappings.size()),
                                              mappings.data()};
    if (rc == AYNI_OK) {
        rc = checked("execution", ayni_register_execution_binding(kernel, &execution));
    }
    return rc;
}
ayni_submit_result_t Motor::submit(void *ctx, const ayni_provider_request_v1 *request,
                                   ayni_provider_completion_t *completion) {
    if (!request || !completion) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    auto &self = *static_cast<Motor *>(ctx);
    if (same(request->capability_id, identities[1])) {
        return self.execution_.submit(request, completion);
    }
    if (same(request->capability_id, identities[2])) {
        // Recoverable soft-stop: confirm GPIO low readback on the control worker and stay armed.
        return self.execution_.submit_soft_stop(request, completion);
    }
    char reply[reply_bytes_max];
    int size = 0;
    if (same(request->capability_id, identities[0])) {
        size = self.execution_.describe(reply, sizeof(reply));
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
} // namespace wifichat
