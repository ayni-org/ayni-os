#include <ayni/applications/normal_fixtures.h>
#include <ayni/ayni.hpp>
#include <ayni/memory.h>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace ayni::applications::normal_fixtures {
namespace {
constexpr uint32_t checkpoint_format = 1;
constexpr uint32_t model_rounds = 3;
constexpr size_t prompt_bytes = 384;
// Fixture capability schema: both authority export and call arguments name
// the same exact simulated destination; Core retains the authorization rules.
ayni_scope_field_v1 remote_field() {
    static const ayni_string_v1 destination{remote_destination, sizeof(remote_destination) - 1};
    auto value = initialized<ayni_authority_value_set_v1>();
    value.kind = AYNI_AUTHORITY_IDENTIFIER;
    value.values.tokens = {&destination, 1, 0};
    return {sizeof(ayni_scope_field_v1), remote_destination_field, value};
}
ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}
bool same(ayni_string_v1 value, const char *expected) {
    return value.size == std::strlen(expected) &&
           std::memcmp(value.data, expected, value.size) == 0;
}
ayni_status_t memory_id(ayni_bytes_v1 bytes, uint64_t &id) {
    if (!bytes.data || !bytes.size) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto *begin = reinterpret_cast<const char *>(bytes.data);
    const auto parsed = std::from_chars(begin, begin + bytes.size, id);
    return parsed.ec == std::errc{} && parsed.ptr == begin + bytes.size && id
               ? AYNI_OK
               : AYNI_INVALID_ARGUMENT;
}
ayni_status_t report(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out,
                     ayni_policy_intent_kind_t kind, const char *key = nullptr) {
    if (!out.intents || !out.intent_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &intent = out.intents[0];
    intent = initialized<ayni_policy_intent_v1>();
    intent.kind = kind;
    intent.expected_revision = s.revision;
    if (key) {
        intent.step_key_size = static_cast<uint32_t>(std::strlen(key));
        std::memcpy(intent.step_key, key, intent.step_key_size);
    }
    out.intent_count = 1;
    return AYNI_OK;
}
ayni_status_t request(ayni_policy_output_v1 &out, ayni_policy_intent_kind_t kind, const char *key,
                      const char *binding = nullptr, ayni_string_v1 body = {}) {
    auto call = initialized<ayni_call_request_v1>();
    call.kind = kind;
    call.step_key = text(key);
    if (binding) {
        call.binding_id = text(binding);
        call.binding_version = 1;
    }
    if (kind != AYNI_POLICY_REQUEST_CALL && kind != AYNI_POLICY_REQUEST_MODEL) {
        return out.request_call ? out.request_call(out.call_context, &call) : AYNI_UNSUPPORTED;
    }
    ValueBuilder<2> value;
    if (kind == AYNI_POLICY_REQUEST_MODEL) {
        value.object();
        value.text(AYNI_MODEL_BODY_FIELD_ID, body);
        value.end();
    } else {
        value.text(0, body);
    }
    const auto status = value.finish(call.value);
    if (status != AYNI_OK) {
        return status;
    }
    return out.request_call ? out.request_call(out.call_context, &call) : AYNI_UNSUPPORTED;
}
ayni_status_t result(const ayni_task_snapshot_v1 &s, const char *key,
                     ayni_task_call_result_v1 &out) {
    out = initialized<ayni_task_call_result_v1>();
    if (!s.call_result) {
        return AYNI_UNSUPPORTED;
    }
    const auto status = s.call_result(s.call_context, text(key), &out);
    if (status != AYNI_OK) {
        return status;
    }
    if (!out.outcome) {
        return AYNI_BUSY;
    }
    if (out.status == AYNI_RESULT_UNKNOWN || out.effect == AYNI_CALL_EFFECT_UNKNOWN ||
        out.outcome == AYNI_CALL_ABANDONED) {
        return AYNI_RESULT_UNKNOWN;
    }
    return out.outcome == AYNI_CALL_COMPLETED ? out.status : AYNI_INVALID_STATE;
}
bool has_step(const ayni_task_snapshot_v1 &s, const char *key) {
    for (uint32_t at = 0; at < s.step_count; ++at) {
        if (same(s.steps[at].step_key, key)) {
            return true;
        }
    }
    return false;
}
// C-CALL-004 / C-POLICY-008: resume from stable step keys, never from a second counter.
ayni_status_t unary(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out) {
    const bool checkpoint = same(s.goal_type_id, "fixture.checkpoint");
    const bool action = same(s.goal_type_id, "fixture.action") || checkpoint;
    if (action) {
        ayni_task_call_result_v1 done{};
        const auto status = result(s, "increment.v1", done);
        if (status == AYNI_NOT_FOUND) {
            return request(out, AYNI_POLICY_REQUEST_CALL, "increment.v1", "increment");
        }
        if (status != AYNI_OK) {
            return status;
        }
        if (!checkpoint) {
            return report(s, out, AYNI_POLICY_REPORT_COMPLETION);
        }
        if (!has_step(s, "checkpoint.ready")) {
            if (!out.checkpoint || out.checkpoint_capacity < 1) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            out.checkpoint[0] = checkpoint_format;
            out.checkpoint_size = 1;
            return report(s, out, AYNI_POLICY_REPORT_PROGRESS, "checkpoint.ready");
        }
    }
    const char *const keys[] = {"model.first.v1", "model.second.v1", "model.third.v1"};
    const uint32_t rounds = same(s.goal_type_id, "fixture.repeat_model") ? model_rounds : 1;
    for (uint32_t at = 0; at < rounds; ++at) {
        ayni_task_call_result_v1 done{};
        const auto status = result(s, keys[at], done);
        if (status == AYNI_NOT_FOUND) {
            return request(out, AYNI_POLICY_REQUEST_MODEL, keys[at], "model",
                           text("fixture input"));
        }
        if (status != AYNI_OK) {
            return status;
        }
    }
    return report(s, out, AYNI_POLICY_REPORT_COMPLETION);
}
// C-SUB-002 / C-PROD-011: capture stays leased while its sample is analyzed.
// Close/STOPPED precedes the notification, whose successful effect precedes Task completion.
ayni_status_t composite(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out) {
    auto subscription = initialized<ayni_subscription_info_v1>();
    auto status = s.subscription(s.call_context, text("capture.v1"), &subscription);
    if (status == AYNI_NOT_FOUND) {
        return request(out, AYNI_POLICY_REQUEST_CALL, "capture.v1", "observe");
    }
    if (status != AYNI_OK) {
        return status;
    }
    ayni_task_call_result_v1 analysis{};
    status = result(s, "analysis.v1", analysis);
    if (status == AYNI_NOT_FOUND) {
        auto sample = initialized<ayni_observation_info_v1>();
        status = s.subscription_observation(s.call_context, text("capture.v1"), &sample);
        if (status == AYNI_NOT_FOUND || (status == AYNI_OK && !sample.usable)) {
            return request(out, AYNI_POLICY_WAIT_SUBSCRIPTION, "capture.v1");
        }
        if (status != AYNI_OK) {
            return status;
        }
        char prompt[prompt_bytes]{};
        const auto count =
            std::snprintf(prompt, sizeof(prompt),
                          "Historical Mock inference only: source=%.*s boot=%" PRIu64
                          " generation=%" PRIu64 " sequence=%" PRIu64 " value=%" PRId64,
                          static_cast<int>(sample.reference.source_id.size),
                          sample.reference.source_id.data, sample.reference.boot_epoch,
                          sample.reference.generation, sample.reference.sequence, sample.value);
        if (count < 0 || static_cast<size_t>(count) >= sizeof(prompt)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        return request(out, AYNI_POLICY_REQUEST_MODEL, "analysis.v1", "model",
                       {prompt, static_cast<uint32_t>(count)});
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (subscription.phase == AYNI_SUBSCRIPTION_ACTIVE ||
        subscription.phase == AYNI_SUBSCRIPTION_STARTING) {
        return request(out, AYNI_POLICY_CLOSE_SUBSCRIPTION, "capture.v1");
    }
    if (subscription.phase != AYNI_SUBSCRIPTION_STOPPED) {
        return request(out, AYNI_POLICY_WAIT_SUBSCRIPTION, "capture.v1");
    }
    ayni_task_call_result_v1 shown{};
    status = result(s, "present.v1", shown);
    if (status == AYNI_NOT_FOUND) {
        return request(out, AYNI_POLICY_REQUEST_CALL, "present.v1", "notify", analysis.text);
    }
    if (status != AYNI_OK) {
        return status;
    }
    return shown.effect == AYNI_CALL_EFFECT_COMPLETE ? report(s, out, AYNI_POLICY_REPORT_COMPLETION)
                                                     : AYNI_INVALID_STATE;
}
} // namespace
const char *goal_type(GoalKind kind) {
    switch (kind) {
    case GoalKind::model:
        return "fixture.model";
    case GoalKind::repeat_model:
        return "fixture.repeat_model";
    case GoalKind::action:
        return "fixture.action";
    case GoalKind::local:
        return "fixture.local";
    case GoalKind::memory_read:
        return "fixture.memory_read";
    case GoalKind::checkpoint:
        return "fixture.checkpoint";
    case GoalKind::composite:
        return "fixture.composite";
    case GoalKind::remote:
        return "fixture.remote";
    }
    return "";
}
ayni_status_t Policy::validate_memory(void *, ayni_bytes_v1 bytes) {
    uint64_t id = 0;
    return memory_id(bytes, id);
}
ayni_status_t Policy::requirements(void *ctx, ayni_bytes_v1,
                                   const ayni_goal_requirement_sink_v1 *sink) {
    const auto kind = *static_cast<const GoalKind *>(ctx);
    if (kind == GoalKind::remote) {
        const auto field = remote_field();
        const ayni_goal_requirement_v1 need{
            sizeof(need), 1, text("test.increment"), {sizeof(ayni_scope_v1), 1, &field}};
        return sink->emit(sink->ctx, &need);
    }
    const char *const needs[] = {"mock.text", "test.increment", "mock.observe", "notify.present"};
    const bool wanted[] = {kind == GoalKind::model || kind == GoalKind::repeat_model ||
                               kind == GoalKind::checkpoint || kind == GoalKind::composite,
                           kind == GoalKind::action || kind == GoalKind::checkpoint,
                           kind == GoalKind::composite, kind == GoalKind::composite};
    for (size_t at = 0; at < sizeof(needs) / sizeof(needs[0]); ++at) {
        if (wanted[at]) {
            const ayni_goal_requirement_v1 need{
                sizeof(need), 1, text(needs[at]), {sizeof(ayni_scope_v1), 0, nullptr}};
            const auto status = sink->emit(sink->ctx, &need);
            if (status != AYNI_OK) {
                return status;
            }
        }
    }
    return AYNI_OK;
}
ayni_status_t Policy::decide(void *, const ayni_task_snapshot_v1 *s, ayni_policy_output_v1 *out) {
    if (s->checkpoint.state.size &&
        (s->checkpoint.state.size != 1 || s->checkpoint.state.data[0] != checkpoint_format)) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (same(s->goal_type_id, "fixture.local")) {
        return report(*s, *out, AYNI_POLICY_REPORT_COMPLETION);
    }
    if (same(s->goal_type_id, "fixture.remote")) {
        auto done = initialized<ayni_task_call_result_v1>();
        const auto status = result(*s, "remote.v1", done);
        if (status == AYNI_NOT_FOUND) {
            auto call = initialized<ayni_call_request_v1>();
            call.kind = AYNI_POLICY_REQUEST_CALL;
            call.step_key = text("remote.v1");
            call.binding_id = text("increment");
            call.binding_version = 1;
            ValueBuilder<2> value;
            value.object();
            value.identifier(remote_destination_field, text(remote_destination));
            value.end();
            const auto built = value.finish(call.value);
            if (built != AYNI_OK) {
                return built;
            }
            return out->request_call(out->call_context, &call);
        }
        return status == AYNI_OK ? report(*s, *out, AYNI_POLICY_REPORT_COMPLETION) : status;
    }
    if (same(s->goal_type_id, "fixture.memory_read")) {
        uint64_t id = 0;
        auto status = memory_id(s->goal_parameters, id);
        if (status != AYNI_OK) {
            return status;
        }
        auto info = initialized<ayni_memory_info_v1>();
        char body[AYNI_MEMORY_BODY_BYTES_DEFAULT]{};
        uint32_t count = 0;
        status = s->memory->read(s->memory->context, id, AYNI_MEMORY_CONTEXT, &info, body,
                                 sizeof(body), &count);
        return status == AYNI_OK ? report(*s, *out, AYNI_POLICY_REPORT_COMPLETION) : status;
    }
    return same(s->goal_type_id, "fixture.composite") ? composite(*s, *out) : unary(*s, *out);
}
ayni_status_t Policy::register_all(ayni_kernel_t *kernel, PolicyRevision revision) {
    const ayni_checkpoint_reader_v1 reader{text("fixture.rules"), 1, 1};
    auto strategy = initialized<ayni_strategy_v1>();
    strategy.strategy_id =
        text(revision == PolicyRevision::v1 ? "fixture.rules" : "fixture.alternate");
    strategy.version = 1;
    strategy.ctx = this;
    strategy.decide = decide;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    strategy.max_checkpoint_bytes = 1;
    if (revision == PolicyRevision::v2) {
        strategy.readers = &reader;
        strategy.reader_count = 1;
    }
    auto status = ayni_register_strategy(kernel, &strategy);
    if (status != AYNI_OK) {
        return status;
    }
    for (auto &kind : kinds_) {
        auto type = initialized<ayni_goal_type_v1>();
        type.version = 1;
        type.goal_type_id = text(goal_type(kind));
        type.strategy_id = strategy.strategy_id;
        type.ctx = &kind;
        type.trigger_kinds = AYNI_TRIGGER_BIT(kind == GoalKind::local ? AYNI_TRIGGER_LOCAL_INPUT
                                                                      : AYNI_TRIGGER_ONCE);
        type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
        type.export_requirements = requirements;
        if (kind == GoalKind::memory_read) {
            type.validate = validate_memory;
        }
        status = ayni_register_goal_type(kernel, &type);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
} // namespace ayni::applications::normal_fixtures
