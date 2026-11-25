#include <array>
#include <ayni/applications/experiment_fixtures.h>
#include <ayni/ayni.hpp>
#include <ayni/memory.h>
#include <charconv>
#include <cstring>
namespace ayni::applications::experiment_fixtures {
namespace {
ayni_string_v1 text(const char *p) {
    return {p, static_cast<uint32_t>(std::strlen(p))};
}
bool same(ayni_string_v1 a, const char *b) {
    return a.size == std::strlen(b) && std::memcmp(a.data, b, a.size) == 0;
}
const char *binding(ayni_string_v1 type) {
    if (same(type, "shared.microphone")) {
        return "microphone";
    }
    if (same(type, "shared.group")) {
        return "group";
    }
    return "observe";
}
ayni_status_t request(ayni_policy_output_v1 &out, ayni_policy_intent_kind_t kind,
                      const char *target) {
    auto call = initialized<ayni_call_request_v1>();
    call.kind = kind;
    call.step_key = text("service.v1");
    if (target) {
        call.binding_id = text(target);
        call.binding_version = 1;
    }
    if (kind != AYNI_POLICY_REQUEST_CALL && kind != AYNI_POLICY_REQUEST_MODEL) {
        return out.request_call ? out.request_call(out.call_context, &call) : AYNI_UNSUPPORTED;
    }
    ValueBuilder<2> value;
    if (kind == AYNI_POLICY_REQUEST_MODEL) {
        value.object();
        value.text(AYNI_MODEL_BODY_FIELD_ID, ayni_string_v1{});
        value.end();
    } else {
        value.text(0, ayni_string_v1{});
    }
    const auto status = value.finish(call.value);
    if (status != AYNI_OK) {
        return status;
    }
    return out.request_call(out.call_context, &call);
}
ayni_status_t requirements(void *ctx, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    const auto *capability = static_cast<const char *>(ctx);
    const ayni_goal_requirement_v1 need{
        sizeof(need), 1, text(capability), {sizeof(ayni_scope_v1), 0, nullptr}};
    return sink->emit(sink->ctx, &need);
}
ayni_status_t source_id(void *, ayni_bytes_v1 bytes) {
    if (!bytes.data || bytes.size == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint64_t id = 0;
    const auto *begin = reinterpret_cast<const char *>(bytes.data);
    const auto parsed = std::from_chars(begin, begin + bytes.size, id);
    return parsed.ec == std::errc{} && parsed.ptr == begin + bytes.size && id != 0
               ? AYNI_OK
               : AYNI_INVALID_ARGUMENT;
}
// C-PROD-041/030: a shared foreground context captures its original read Grant
// through the model result and derived proposal; background ORGANIZE stays owner-only.
ayni_status_t shared_summary(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out) {
    uint64_t id = 0;
    const auto checked = source_id(nullptr, s.goal_parameters);
    if (checked != AYNI_OK) {
        return checked;
    }
    const auto *begin = reinterpret_cast<const char *>(s.goal_parameters.data);
    std::from_chars(begin, begin + s.goal_parameters.size, id);
    std::array<char, 256> source{};
    uint32_t size = 0;
    auto memory = initialized<ayni_memory_info_v1>();
    auto status = s.memory->read(s.memory->context, id, AYNI_MEMORY_CONTEXT, &memory, source.data(),
                                 source.size(), &size);
    if (status != AYNI_OK) {
        return status;
    }
    auto result = initialized<ayni_task_call_result_v1>();
    status = s.call_result(s.call_context, text("shared.summary.v1"), &result);
    if (status == AYNI_NOT_FOUND) {
        ValueBuilder<2> value;
        value.object();
        value.text(AYNI_MODEL_BODY_FIELD_ID, {source.data(), size});
        value.end();
        ayni_call_request_v1 call{
            sizeof(call), AYNI_POLICY_REQUEST_MODEL, 1, 0, text("shared.summary.v1"), text("model"),
            {},           AYNI_CALL_MODE_FOREGROUND, 0};
        const auto built = value.finish(call.value);
        if (built != AYNI_OK) {
            return built;
        }
        return out.request_call(out.call_context, &call);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (result.outcome != AYNI_CALL_COMPLETED) {
        return AYNI_RESULT_UNKNOWN;
    }
    if (s.step_count > 1) {
        auto &intent = out.intents[0];
        intent = initialized<ayni_policy_intent_v1>();
        intent.kind = AYNI_POLICY_REPORT_COMPLETION;
        intent.expected_revision = s.revision;
        out.intent_count = 1;
        return AYNI_OK;
    }
    const auto key = text("shared.candidate.v1");
    auto proposal = initialized<ayni_memory_proposal_v1>();
    proposal.source = AYNI_MEMORY_MODEL;
    proposal.source_call_id = result.logical_call_id;
    proposal.step_key = key;
    proposal.body = result.text;
    status = s.memory->propose(s.memory->context, &proposal);
    if (status != AYNI_OK) {
        return status;
    }
    auto &intent = out.intents[0];
    intent = initialized<ayni_policy_intent_v1>();
    intent.kind = AYNI_POLICY_WRITE_MEMORY;
    intent.expected_revision = s.revision;
    intent.step_key_size = key.size;
    std::memcpy(intent.step_key, key.data, key.size);
    out.intent_count = 1;
    return AYNI_OK;
}
} // namespace
// C-RES-001 / C-SUB-002: ordinary calls finish only from their original result;
// an observation Goal deliberately waits with its lease until owner cancellation.
ayni_status_t Services::decide(void *, const ayni_task_snapshot_v1 *s, ayni_policy_output_v1 *out) {
    if (same(s->goal_type_id, "shared.context_summary")) {
        return shared_summary(*s, *out);
    }
    if (same(s->goal_type_id, "shared.priority_rule")) {
        auto &intent = out->intents[0];
        intent = initialized<ayni_policy_intent_v1>();
        intent.kind = AYNI_POLICY_REPORT_COMPLETION;
        intent.expected_revision = s->revision;
        out->intent_count = 1;
        return AYNI_OK;
    }
    const auto *target = binding(s->goal_type_id);
    if (same(s->goal_type_id, "shared.observe")) {
        auto sub = initialized<ayni_subscription_info_v1>();
        const auto status = s->subscription(s->call_context, text("service.v1"), &sub);
        return status == AYNI_NOT_FOUND ? request(*out, AYNI_POLICY_REQUEST_CALL, target)
               : status == AYNI_OK      ? request(*out, AYNI_POLICY_WAIT_SUBSCRIPTION, nullptr)
                                        : status;
    }
    auto result = initialized<ayni_task_call_result_v1>();
    const auto status = s->call_result(s->call_context, text("service.v1"), &result);
    if (status == AYNI_NOT_FOUND) {
        return request(*out, AYNI_POLICY_REQUEST_CALL, target);
    }
    if (status != AYNI_OK) {
        return status;
    }
    auto &intent = out->intents[0];
    intent = initialized<ayni_policy_intent_v1>();
    intent.kind = result.outcome == AYNI_CALL_COMPLETED ? AYNI_POLICY_REPORT_COMPLETION
                                                        : AYNI_POLICY_REPORT_FAILURE;
    intent.expected_revision = s->revision;
    out->intent_count = 1;
    return AYNI_OK;
}
ayni_status_t Services::register_all(ayni_kernel_t *kernel) {
    auto policy = initialized<ayni_strategy_v1>();
    policy.strategy_id = text("experiment.services");
    policy.version = 1;
    policy.max_intents_per_call = 1;
    policy.intent_size = sizeof(ayni_policy_intent_v1);
    policy.decide = decide;
    auto status = ayni_register_strategy(kernel, &policy);
    static const char *const types[] = {"shared.microphone", "shared.group", "shared.observe",
                                        "shared.context_summary"};
    static const char *const capabilities[] = {"test.microphone", "test.group", "mock.observe",
                                               "mock.text"};
    for (uint32_t at = 0; at < sizeof(types) / sizeof(types[0]) && status == AYNI_OK; ++at) {
        auto goal = initialized<ayni_goal_type_v1>();
        goal.version = 1;
        goal.goal_type_id = text(types[at]);
        goal.strategy_id = policy.strategy_id;
        goal.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
        goal.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
        if (same(goal.goal_type_id, "shared.context_summary")) {
            goal.validate = source_id;
        }
        goal.ctx = const_cast<char *>(capabilities[at]);
        goal.export_requirements = requirements;
        status = ayni_register_goal_type(kernel, &goal);
    }
    if (status != AYNI_OK) {
        return status;
    }
    auto rule = initialized<ayni_goal_type_v1>();
    rule.version = 1;
    rule.goal_type_id = text("shared.priority_rule");
    rule.strategy_id = policy.strategy_id;
    rule.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_LOCAL_INPUT);
    rule.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    rule.work_profile.work_class = AYNI_WORK_FOREGROUND;
    return ayni_register_goal_type(kernel, &rule);
}
} // namespace ayni::applications::experiment_fixtures
