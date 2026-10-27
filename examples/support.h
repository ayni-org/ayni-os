// Shared public-API mechanics for the four small example clients (ADR-0067).
#ifndef AYNI_EXAMPLE_SUPPORT_H
#define AYNI_EXAMPLE_SUPPORT_H
#include <ayni/authority.h>
#include <ayni/ayni.hpp>
#include <ayni/calls.h>
#include <ayni/runtime/examples.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace ayni::examples {
inline ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
inline ayni_scope_v1 empty_scope() {
    return {sizeof(ayni_scope_v1), 0, nullptr};
}
inline bool equal(ayni_string_v1 a, const char *b) {
    const auto size = std::strlen(b);
    return a.size == size && (!size || (a.data && std::memcmp(a.data, b, size) == 0));
}
inline ayni_status_t report(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &out,
                            ayni_policy_intent_kind_t kind) {
    if (!out.intents || !out.intent_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out.intents[0] = initialized<ayni_policy_intent_v1>();
    out.intents[0].kind = kind;
    out.intents[0].expected_revision = s.revision;
    out.intent_count = 1;
    return AYNI_OK;
}
inline ayni_status_t request(ayni_policy_output_v1 &out, ayni_policy_intent_kind_t kind,
                             const char *key, const char *binding = nullptr,
                             ayni_string_v1 body = {}, ayni_scope_v1 scope = empty_scope()) {
    if (!out.request_call) {
        return AYNI_UNSUPPORTED;
    }
    auto call = initialized<ayni_call_request_v1>();
    call.kind = kind;
    call.step_key = text(key);
    if (binding) {
        call.binding_id = text(binding);
        call.binding_version = 1;
        call.arguments = scope;
        call.text = body;
    }
    return out.request_call(out.call_context, &call);
}
inline ayni_status_t result(const ayni_task_snapshot_v1 &s, const char *key,
                            ayni_task_call_result_v1 &out) {
    if (!s.call_result) {
        return AYNI_UNSUPPORTED;
    }
    out = initialized<ayni_task_call_result_v1>();
    const auto status = s.call_result(s.call_context, text(key), &out);
    if (status != AYNI_OK) {
        return status;
    }
    if (!out.outcome) {
        return AYNI_BUSY;
    }
    if (out.outcome == AYNI_CALL_ABANDONED || out.status == AYNI_RESULT_UNKNOWN ||
        out.effect == AYNI_CALL_EFFECT_UNKNOWN) {
        return AYNI_RESULT_UNKNOWN;
    }
    if (out.outcome != AYNI_CALL_COMPLETED || out.status != AYNI_OK) {
        return out.status == AYNI_OK ? AYNI_INVALID_STATE : out.status;
    }
    return AYNI_OK;
}
inline ayni_status_t install(ayni_kernel_t *kernel, const char *type_id, const char *strategy_id,
                             void *context, ayni_decide_fn decide,
                             ayni_goal_requirements_fn requirements = nullptr,
                             ayni_trigger_kind_t trigger = AYNI_TRIGGER_ONCE) {
    auto strategy = initialized<ayni_strategy_v1>();
    strategy.version = 1;
    strategy.strategy_id = text(strategy_id);
    strategy.ctx = context;
    strategy.decide = decide;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    auto status = ayni_register_strategy(kernel, &strategy);
    if (status != AYNI_OK) {
        return status;
    }
    auto type = initialized<ayni_goal_type_v1>();
    type.version = 1;
    type.goal_type_id = text(type_id);
    type.strategy_id = strategy.strategy_id;
    type.ctx = context;
    type.trigger_kinds = AYNI_TRIGGER_BIT(trigger);
    type.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
    type.export_requirements = requirements;
    return ayni_register_goal_type(kernel, &type);
}
inline ayni_goal_create_v1 goal(runtime::examples::Fixture &fixture, const char *type) {
    auto out = initialized<ayni_goal_create_v1>();
    out.goal_type_id = text(type);
    out.goal_type_version = 1;
    out.owner_instance_id = fixture.instance();
    out.trigger = AYNI_TRIGGER_ONCE;
    return out;
}
inline int finish(const char *name, ayni_status_t status, runtime::examples::Fixture &fixture) {
    const auto closed = fixture.close();
    if (closed != AYNI_OK) {
        // External adapter/policy contexts still live on main's stack. Stop the
        // standalone example before unwinding them under retained Kernel borrows.
        std::fprintf(stderr, "%s: failed drain (status=%d)\n", name, closed);
        std::fflush(stderr);
        std::_Exit(1);
    }
    if (status == AYNI_OK) {
        status = closed;
    }
    std::printf("%s: %s (status=%d)\n", name, status == AYNI_OK ? "passed" : "failed", status);
    return status == AYNI_OK ? 0 : 1;
}
} // namespace ayni::examples
#endif
