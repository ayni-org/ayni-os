#include "motor.h"
#include "motor_plan.h"
#include <ayni/directory.h>
#include <ayni/values.hpp>
#include <cstdio>
#include <cstring>
namespace usbchat {
namespace {
constexpr uint32_t version = 1, reply_bytes_max = 1024, input_bytes_max = 256;
constexpr uint32_t motor1_field = 1, motor2_field = 2, duration_field = 3;
constexpr uint32_t request_timeout_ms = 1000, reported_supply_mv = 10000;
constexpr const char *identities[] = {"motor.status", "motor.plan"};
ayni_string_v1 text(const char *s) {
    return {s, uint32_t(std::strlen(s))};
}
bool same(ayni_string_v1 a, const char *b) {
    return a.size == std::strlen(b) && !std::memcmp(a.data, b, a.size);
}
} // namespace
ayni_status_t Motor::configure(ayni_kernel_t *kernel) {
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
    for (uint32_t at = 0; at < offers_.size(); ++at) {
        const ayni_capability_desc_v1 authority{
            sizeof(authority), version, text(identities[at]), 0, 0, nullptr};
        auto rc = ayni_register_capability(kernel, &authority);
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
        rc = ayni_register_call_capability(kernel, &offers_[at]);
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    provider_ = {sizeof(provider_),
                 version,
                 text("motor.software"),
                 text("motor.ta6586"),
                 {},
                 this,
                 uint32_t(offers_.size()),
                 0,
                 offers_.data(),
                 submit,
                 nullptr,
                 nullptr,
                 nullptr};
    auto rc = ayni_register_provider(kernel, &provider_);
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
        rc = ayni_register_binding(kernel, &binding);
        if (rc != AYNI_OK) {
            break;
        }
        const bool plan = at != 0;
        const ayni_tool_usage_v1 usage{
            sizeof(usage),
            version,
            binding.binding_id,
            version,
            binding.capability_id,
            text(plan
                     ? "Compute a TA6586 dual-motor software plan. Never drives GPIO or motors."
                     : "Read configured TA6586 wiring and software readiness, not measured power."),
            text(plan ? "Required motor1_percent and motor2_percent: integers -100..100; "
                        "duration_ms: 1..5000. Zero coasts. Positive means D0/D2 PWM, "
                        "negative D1/D3 PWM; shaft direction is not calibrated."
                      : "Empty object. Wiring and 10000 mV motor supply are user configuration, "
                        "not voltage measurements."),
            text("motion_enabled=false and executed=false. Planning success is not movement "
                 "success. Real motion requires the physical execution/independent-stop "
                 "adapter and hardware validation; those are not enabled."),
            text(plan ? "{\"motor1_percent\":20,\"motor2_percent\":-20,\"duration_ms\":300}"
                      : "{}"),
            0,
            0,
            nullptr};
        rc = ayni_register_capability_usage(
            kernel,
            text(plan ? "Plan TA6586 D0..D3 levels without executing motion"
                      : "Query TA6586 wiring; real motion is disabled"),
            &usage);
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
ayni_submit_result_t Motor::submit(void *, const ayni_provider_request_v1 *request,
                                   ayni_provider_completion_t *completion) {
    if (!request || !completion) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    char reply[reply_bytes_max];
    int size = 0;
    if (same(request->capability_id, identities[0])) {
        size = std::snprintf(
            reply, sizeof(reply),
            "{\"driver\":\"TA6586\",\"driver_source\":\"user-confirmed-chip-marking\","
            "\"gpio_d0_d1_d2_d3\":[%lu,%lu,%lu,%lu],"
            "\"configured_supply_mv\":%lu,\"supply_source\":\"user-reported\","
            "\"aux_3v3\":\"board-output\",\"power_state\":\"not-measured\","
            "\"motion_enabled\":false,\"executed\":false,"
            "\"mode\":\"software-planning-only\",\"stop_chain\":\"not-configured\"}",
            static_cast<unsigned long>(motor_pins[0]), static_cast<unsigned long>(motor_pins[1]),
            static_cast<unsigned long>(motor_pins[2]), static_cast<unsigned long>(motor_pins[3]),
            static_cast<unsigned long>(reported_supply_mv));
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
} // namespace usbchat
