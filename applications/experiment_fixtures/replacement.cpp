// E3 / C-POLICY-008: both policies use the same action/model identities. The
// ToolLoop owns its private format; the rule policy writes an empty checkpoint.
#include <ayni/applications/experiment_fixtures.h>
#include <ayni/ayni.hpp>
#include <cstring>
namespace ayni::applications::experiment_fixtures::replacement {
namespace {
constexpr char rule_id[] = "replacement.rules";
constexpr char workflow_id[] = "replacement.workflow";
constexpr char model_id[] = "replacement.model";
ayni_string_v1 text(const char *v) {
    return {v, static_cast<uint32_t>(std::strlen(v))};
}
bool equal(ayni_string_v1 v, const char *s) {
    return v.size == std::strlen(s) && std::memcmp(v.data, s, v.size) == 0;
}
ayni_status_t report(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &o,
                     const char *key = nullptr) {
    auto &i = o.intents[0];
    i = initialized<ayni_policy_intent_v1>();
    i.kind = key ? AYNI_POLICY_REPORT_PROGRESS : AYNI_POLICY_REPORT_COMPLETION;
    i.expected_revision = s.revision;
    if (key) {
        i.step_key_size = text(key).size;
        std::memcpy(i.step_key, key, i.step_key_size);
    }
    o.intent_count = 1;
    return AYNI_OK;
}
ayni_status_t request(ayni_policy_output_v1 &o, const char *key, const char *binding,
                      ayni_policy_intent_kind_t kind) {
    auto r = initialized<ayni_call_request_v1>();
    r.kind = kind;
    r.step_key = text(key);
    r.binding_id = text(binding);
    r.binding_version = 1;
    ValueBuilder<2> value;
    if (kind == AYNI_POLICY_REQUEST_MODEL) {
        value.object();
        value.text(AYNI_MODEL_BODY_FIELD_ID, text("ayni-instructions:0\nfixture input"));
        value.end();
    } else {
        value.text(0, {});
    }
    const auto status = value.finish(r.value);
    if (status != AYNI_OK) {
        return status;
    }
    return o.request_call(o.call_context, &r);
}
ayni_status_t result(const ayni_task_snapshot_v1 &s, const char *key) {
    auto r = initialized<ayni_task_call_result_v1>();
    const auto status = s.call_result(s.call_context, text(key), &r);
    if (status != AYNI_OK) {
        return status;
    }
    if (!r.outcome) {
        return AYNI_BUSY;
    }
    if (r.effect == AYNI_CALL_EFFECT_UNKNOWN || r.status == AYNI_RESULT_UNKNOWN) {
        return AYNI_RESULT_UNKNOWN;
    }
    return r.outcome == AYNI_CALL_COMPLETED ? r.status : AYNI_INVALID_STATE;
}
// The stable prerequisite is common to both strategies, independent of private
// bytes. A committed action is read again, never reissued under another key.
} // namespace
ayni_status_t prerequisite(const ayni_task_snapshot_v1 &s, ayni_policy_output_v1 &o,
                           bool &handled) {
    handled = false;
    if (!equal(s.goal_type_id, workflow_id)) {
        return AYNI_OK;
    }
    const auto status = result(s, "increment");
    if (status != AYNI_OK) {
        handled = true;
        return status == AYNI_NOT_FOUND
                   ? request(o, "increment", "increment", AYNI_POLICY_REQUEST_CALL)
                   : status;
    }
    for (uint32_t i = 0; i < s.step_count; ++i) {
        if (equal(s.steps[i].step_key, "checkpoint.ready")) {
            return AYNI_OK;
        }
    }
    handled = true;
    return report(s, o, "checkpoint.ready");
}
namespace {
ayni_status_t rules(void *, const ayni_task_snapshot_v1 *s, ayni_policy_output_v1 *o) {
    if (s->checkpoint.state.size) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    bool handled = false;
    auto status = prerequisite(*s, *o, handled);
    if (handled || status != AYNI_OK) {
        return status;
    }
    status = result(*s, "model.0");
    if (status == AYNI_NOT_FOUND) {
        return request(*o, "model.0", "model", AYNI_POLICY_REQUEST_MODEL);
    }
    return status == AYNI_OK ? report(*s, *o) : status;
}
ayni_status_t requirements(void *, ayni_bytes_v1, const ayni_goal_requirement_sink_v1 *sink) {
    // Both types declare the superset so replacement never acquires new grants.
    for (const char *id : {"test.increment", "mock.text"}) {
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
ayni_strategy_v1 rule_strategy() {
    auto strategy = initialized<ayni_strategy_v1>();
    strategy.strategy_id = text(rule_id);
    strategy.version = 1;
    strategy.decide = rules;
    strategy.max_intents_per_call = 1;
    strategy.intent_size = sizeof(ayni_policy_intent_v1);
    return strategy;
}
ayni_status_t register_goals(ayni_kernel_t *k, ayni_string_v1 strategy_id,
                             ayni_status_t (*validate)(void *, ayni_bytes_v1),
                             void *validation_context) {
    auto status = AYNI_OK;
    for (const char *id : {workflow_id, model_id}) {
        if (status != AYNI_OK) {
            return status;
        }
        auto goal = initialized<ayni_goal_type_v1>();
        goal.goal_type_id = text(id);
        goal.version = 1;
        goal.strategy_id = strategy_id;
        goal.trigger_kinds = AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE);
        goal.occurrence_wait_max_us = AYNI_OCCURRENCE_WAIT_UNBOUNDED;
        goal.ctx = validation_context;
        goal.validate = validate;
        goal.export_requirements = requirements;
        status = ayni_register_goal_type(k, &goal);
    }
    return status;
}
} // namespace ayni::applications::experiment_fixtures::replacement
