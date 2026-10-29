// E4 / C-CALL-004, C-SUB-002, C-PROD-030: ordered application semantics use
// Core-owned results/steps. A policy never treats UNKNOWN as a completed step.
#include <array>
#include <ayni/applications/experiment_fixtures.h>
#include <ayni/ayni.hpp>
#include <ayni/memory.h>
#include <ayni/observations.h>
#include <charconv>
#include <cstdio>
#include <cstring>
namespace ayni::applications::experiment_fixtures::composite {
namespace {
constexpr uint32_t source_bytes = 128, prompt_bytes = 512;
constexpr uint32_t checkpoint_bytes = 32;
constexpr uint32_t compatible_version = 2;
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
bool same(ayni_string_v1 a, const char *b) {
    return a.size == std::strlen(b) && std::memcmp(a.data, b, a.size) == 0;
}
bool step(const ayni_task_snapshot_v1 &s, const char *key) {
    for (uint32_t i = 0; i < s.step_count; ++i) {
        if (same(s.steps[i].step_key, key)) {
            return true;
        }
    }
    return false;
}
ayni_status_t source_id(ayni_bytes_v1 bytes, uint64_t &id) {
    if (!bytes.data || !bytes.size) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto begin = reinterpret_cast<const char *>(bytes.data);
    const auto read = std::from_chars(begin, begin + bytes.size, id);
    return read.ec == std::errc{} && read.ptr == begin + bytes.size && id ? AYNI_OK
                                                                          : AYNI_INVALID_ARGUMENT;
}
ayni_status_t validate(void *, ayni_bytes_v1 bytes) {
    uint64_t id = 0;
    return source_id(bytes, id);
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
    return out.request_call(out.call_context, &call);
}
ayni_status_t result(const ayni_task_snapshot_v1 &s, const char *key,
                     ayni_task_call_result_v1 &value) {
    value = initialized<ayni_task_call_result_v1>();
    const auto status = s.call_result(s.call_context, text(key), &value);
    if (status != AYNI_OK) {
        return status;
    }
    if (value.effect == AYNI_CALL_EFFECT_UNKNOWN || value.status == AYNI_RESULT_UNKNOWN) {
        return AYNI_RESULT_UNKNOWN;
    }
    if (value.outcome == AYNI_CALL_COMPLETED || value.status != AYNI_OK) {
        return value.status; // Preserve the classified Provider failure through the policy hold.
    }
    return AYNI_INVALID_STATE;
}
ayni_status_t read_source(const ayni_task_snapshot_v1 &s, char *body, uint32_t &size) {
    uint64_t id = 0;
    auto status = source_id(s.goal_parameters, id);
    auto info = initialized<ayni_memory_info_v1>();
    return status == AYNI_OK ? s.memory->read(s.memory->context, id, AYNI_MEMORY_CONTEXT, &info,
                                              body, source_bytes, &size)
                             : status;
}
ayni_status_t analyze(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out) {
    char source[source_bytes]{};
    uint32_t size = 0;
    auto status = read_source(s, source, size);
    if (status != AYNI_OK) {
        return status;
    }
    auto sample = initialized<ayni_observation_info_v1>();
    status = s.subscription_observation(s.call_context, text("capture.v1"), &sample);
    if (status == AYNI_NOT_FOUND || (status == AYNI_OK && !sample.usable)) {
        return request(out, AYNI_POLICY_WAIT_SUBSCRIPTION, "capture.v1");
    }
    if (status != AYNI_OK) {
        return status;
    }
    // The private format is bounded decimal generation, portable across Hosts.
    const auto encoded =
        std::to_chars(reinterpret_cast<char *>(out.checkpoint),
                      reinterpret_cast<char *>(out.checkpoint) + out.checkpoint_capacity,
                      sample.reference.generation);
    if (encoded.ec != std::errc{}) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out.checkpoint_size =
        static_cast<uint32_t>(encoded.ptr - reinterpret_cast<char *>(out.checkpoint));
    char prompt[prompt_bytes]{};
    const auto count =
        std::snprintf(prompt, sizeof(prompt),
                      "Historical capture source=%.*s version=%u boot=%llu generation=%llu "
                      "sequence=%llu value=%lld. Memory: %.*s",
                      static_cast<int>(sample.reference.source_id.size),
                      sample.reference.source_id.data, sample.reference.source_version,
                      static_cast<unsigned long long>(sample.reference.boot_epoch),
                      static_cast<unsigned long long>(sample.reference.generation),
                      static_cast<unsigned long long>(sample.reference.sequence),
                      static_cast<long long>(sample.value), static_cast<int>(size), source);
    if (count < 0 || static_cast<size_t>(count) >= sizeof(prompt)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return request(out, AYNI_POLICY_REQUEST_MODEL, "analysis.v1", "model",
                   {prompt, static_cast<uint32_t>(count)});
}
ayni_status_t summarize(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out,
                        const ayni_task_call_result_v1 &analysis) {
    char source[source_bytes]{};
    uint32_t size = 0;
    auto status = read_source(s, source, size);
    if (status != AYNI_OK) {
        return status;
    }
    auto proposal = initialized<ayni_memory_proposal_v1>();
    proposal.source = AYNI_MEMORY_MODEL;
    proposal.source_call_id = analysis.logical_call_id;
    proposal.step_key = text("summary.v1");
    proposal.body = analysis.text;
    status = s.memory->propose(s.memory->context, &proposal);
    if (status != AYNI_OK) {
        return status;
    }
    auto &intent = out.intents[0];
    intent = initialized<ayni_policy_intent_v1>();
    intent.kind = AYNI_POLICY_WRITE_MEMORY;
    intent.expected_revision = s.revision;
    intent.step_key_size = proposal.step_key.size;
    std::memcpy(intent.step_key, proposal.step_key.data, proposal.step_key.size);
    out.intent_count = 1;
    return AYNI_OK;
}
ayni_status_t oversized_round(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out) {
    ayni_task_call_result_v1 answer{};
    const auto status = result(s, "round.v1", answer);
    if (status != AYNI_NOT_FOUND) {
        return status == AYNI_OK ? AYNI_INVALID_STATE : status;
    }
    constexpr char proposal[] = "call:notify ";
    constexpr uint32_t proposal_bytes = sizeof(proposal) - 1;
    constexpr uint32_t excessive_count = AYNI_CALL_TOOL_COUNT_MAX + 1;
    std::array<char, proposal_bytes * excessive_count> body{};
    for (uint32_t i = 0; i < excessive_count; ++i) {
        std::memcpy(body.data() + i * proposal_bytes, proposal, proposal_bytes);
    }
    return request(out, AYNI_POLICY_REQUEST_MODEL, "round.v1", "tools.model",
                   {body.data(), static_cast<uint32_t>(body.size())});
}
ayni_status_t complete(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out) {
    auto &intent = out.intents[0];
    intent = initialized<ayni_policy_intent_v1>();
    intent.kind = AYNI_POLICY_REPORT_COMPLETION;
    intent.expected_revision = s.revision;
    out.intent_count = 1;
    return AYNI_OK;
}
// Completed action -> live capture -> analysis -> durable derived memory ->
// confirmed capture close -> presentation. Common steps survive reader changes.
ayni_status_t decide(void *, const ayni_task_snapshot_v1 *s, ayni_policy_output_v1 *out) {
    if (same(s->goal_type_id, "composite.read_source")) {
        char source[source_bytes]{};
        uint32_t size = 0;
        const auto status = read_source(*s, source, size);
        return status == AYNI_OK ? complete(*s, *out) : status;
    }
    if (same(s->goal_type_id, "composite.round")) {
        return oversized_round(*s, *out);
    }
    ayni_task_call_result_v1 action{}, analysis{}, presented{};
    auto status = result(*s, "increment.v1", action);
    if (status == AYNI_NOT_FOUND) {
        return request(*out, AYNI_POLICY_REQUEST_CALL, "increment.v1", "increment");
    }
    if (status != AYNI_OK) {
        return status;
    }
    auto lease = initialized<ayni_subscription_info_v1>();
    status = s->subscription(s->call_context, text("capture.v1"), &lease);
    if (status == AYNI_NOT_FOUND) {
        return request(*out, AYNI_POLICY_REQUEST_CALL, "capture.v1", "observe");
    }
    if (status != AYNI_OK) {
        return status;
    }
    status = result(*s, "analysis.v1", analysis);
    if (status == AYNI_NOT_FOUND) {
        return analyze(*s, *out);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (!step(*s, "summary.v1")) {
        // C-SUB-002: a closed or replaced generation cannot authorize a late
        // candidate. The policy's own close is issued only after summary commits.
        uint64_t generation = 0;
        const auto parsed = source_id(s->checkpoint.state, generation);
        // A durable completed analysis is historical inference; after restart a
        // fresh live capture may coexist with it. It never becomes motion evidence.
        if (parsed != AYNI_OK || lease.phase != AYNI_SUBSCRIPTION_ACTIVE) {
            return AYNI_EXPIRED;
        }
        return summarize(*s, *out, analysis);
    }
    if (lease.phase == AYNI_SUBSCRIPTION_ACTIVE || lease.phase == AYNI_SUBSCRIPTION_STARTING) {
        return request(*out, AYNI_POLICY_CLOSE_SUBSCRIPTION, "capture.v1");
    }
    if (lease.phase != AYNI_SUBSCRIPTION_STOPPED) {
        return request(*out, AYNI_POLICY_WAIT_SUBSCRIPTION, "capture.v1");
    }
    status = result(*s, "present.v1", presented);
    if (status == AYNI_NOT_FOUND) {
        return request(*out, AYNI_POLICY_REQUEST_CALL, "present.v1", "notify", analysis.text);
    }
    if (status != AYNI_OK) {
        return status;
    }
    if (presented.effect != AYNI_CALL_EFFECT_COMPLETE) {
        return AYNI_RESULT_UNKNOWN;
    }
    return complete(*s, *out);
}
ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    for (const char *id : {"test.increment", "mock.observe", "mock.text", "notify.present"}) {
        const ayni_goal_requirement_v1 need{
            sizeof(need), 1, text(id), {sizeof(ayni_scope_v1), 0, nullptr}};
        const auto status = sink->emit(sink->ctx, &need);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
ayni_status_t parent_requirements(void *ctx, ayni_bytes_v1 bytes,
                                  const ayni_goal_requirement_sink_v1 *sink) {
    const auto status = requirements(ctx, bytes, sink);
    const ayni_goal_requirement_v1 need{
        sizeof(need), 1, text("mock.turn"), {sizeof(ayni_scope_v1), 0, nullptr}};
    return status == AYNI_OK ? sink->emit(sink->ctx, &need) : status;
}
ayni_status_t round_requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    for (const char *id : {"mock.tools", "notify.present"}) {
        const ayni_goal_requirement_v1 need{
            sizeof(need), 1, text(id), {sizeof(ayni_scope_v1), 0, nullptr}};
        const auto status = sink->emit(sink->ctx, &need);
        if (status != AYNI_OK) {
            return status;
        }
    }
    return AYNI_OK;
}
} // namespace
ayni_status_t register_workflow(ayni_kernel_t *kernel, uint32_t version, uint32_t goal_version) {
    static const ayni_checkpoint_reader_v1 reader{text("composite.policy"), 1, compatible_version};
    auto strategy = initialized<ayni_strategy_v1>();
    strategy.strategy_id = reader.strategy_id;
    strategy.version = version;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    strategy.max_checkpoint_bytes = checkpoint_bytes;
    strategy.reader_count = 1;
    strategy.readers = &reader;
    strategy.decide = decide;
    auto status = ayni_register_strategy(kernel, &strategy);
    if (status != AYNI_OK) {
        return status;
    }
    auto goal = initialized<ayni_goal_type_v1>();
    goal.version = goal_version;
    goal.goal_type_id = text("composite_workflow");
    goal.strategy_id = strategy.strategy_id;
    goal.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    goal.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    goal.validate = validate;
    goal.export_requirements = requirements;
    status = ayni_register_goal_type(kernel, &goal);
    if (status != AYNI_OK) {
        return status;
    }
    // A parent declares the full authority ceiling before children are accepted;
    // an empty folder cannot be used to expand a child's authority (C-LIFE-022).
    goal.version = 1;
    goal.goal_type_id = text("composite.parent");
    goal.strategy_id = {};
    goal.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE);
    goal.validate = nullptr;
    goal.export_requirements = parent_requirements;
    status = ayni_register_goal_type(kernel, &goal);
    if (status != AYNI_OK) {
        return status;
    }
    static const ayni_tool_ref_v1 notify{text("notify"), 1, 0};
    goal.goal_type_id = text("composite.round");
    goal.strategy_id = strategy.strategy_id;
    goal.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
    goal.export_requirements = round_requirements;
    goal.tool_count = 1;
    goal.tools = &notify;
    status = ayni_register_goal_type(kernel, &goal);
    if (status != AYNI_OK) {
        return status;
    }
    goal.goal_type_id = text("composite.read_source");
    goal.validate = validate;
    goal.export_requirements = nullptr;
    goal.tool_count = 0;
    goal.tools = nullptr;
    return ayni_register_goal_type(kernel, &goal);
}
} // namespace ayni::applications::experiment_fixtures::composite
