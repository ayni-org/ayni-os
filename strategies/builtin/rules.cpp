#include "support.h"

namespace ayni::strategies {
namespace {
using namespace detail;
bool step_key(const RuleStep &step, char *buffer, ayni_string_v1 &key) {
    Writer writer(buffer, AYNI_CATALOG_ID_MAX_BYTES);
    writer.add(step.name);
    writer.add(".v");
    writer.number(step.version);
    key = writer.text();
    return writer.ok();
}
bool valid_rule(const Rule &rule) {
    if (!valid_id(rule.goal_type_id) || !rule.goal_type_version || !rule.steps ||
        !rule.step_count || rule.step_count > rule_steps_max) {
        return false;
    }
    for (uint32_t at = 0; at < rule.step_count; ++at) {
        const auto &step = rule.steps[at];
        char key[AYNI_CATALOG_ID_MAX_BYTES];
        ayni_string_v1 name{};
        if (!valid_id(step.name) || !step.version || !valid_call(step.call) ||
            !step_key(step, key, name)) {
            return false;
        }
        for (uint32_t before = 0; before < at; ++before) {
            if (equal(step.name, rule.steps[before].name) &&
                step.version == rule.steps[before].version) {
                return false;
            }
        }
    }
    return true;
}
} // namespace
ayni_status_t Rules::configure(Identity identity, const Rule *rules, uint32_t count) {
    if (rules_) {
        return AYNI_INVALID_STATE;
    }
    if (!valid_identity(identity) || !rules || !count || count > rule_count_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < count; ++at) {
        if (!valid_rule(rules[at])) {
            return AYNI_INVALID_ARGUMENT;
        }
        for (uint32_t before = 0; before < at; ++before) {
            if (equal(rules[before].goal_type_id, rules[at].goal_type_id) &&
                rules[before].goal_type_version == rules[at].goal_type_version) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
    }
    identity_ = identity;
    rules_ = rules;
    count_ = count;
    return AYNI_OK;
}
ayni_strategy_v1 Rules::describe() {
    return descriptor(identity_, this, decide, 0);
}
// Calls are the common progress ledger. A known completed call is never sent
// again after a strategy replacement, and no private cursor shadows its state.
ayni_status_t Rules::decide(void *context, const ayni_task_snapshot_v1 *snapshot,
                            ayni_policy_output_v1 *output) {
    if (!context || !snapshot || !output) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &self = *static_cast<const Rules *>(context);
    if (!self.rules_) {
        return AYNI_INVALID_STATE;
    }
    if (snapshot->checkpoint.state.size) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    const Rule *selected = nullptr;
    for (uint32_t at = 0; at < self.count_; ++at) {
        if (equal(self.rules_[at].goal_type_id, snapshot->goal_type_id) &&
            self.rules_[at].goal_type_version == snapshot->goal_type_version) {
            selected = &self.rules_[at];
        }
    }
    if (!selected) {
        return AYNI_NOT_FOUND;
    }
    if (selected->step_count > snapshot->step_capacity) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    for (uint32_t at = 0; at < selected->step_count; ++at) {
        const auto &step = selected->steps[at];
        char buffer[AYNI_CATALOG_ID_MAX_BYTES];
        ayni_string_v1 key{};
        if (!step_key(step, buffer, key)) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        ayni_task_call_result_v1 call{};
        const auto status = result(*snapshot, key, call);
        if (status == AYNI_NOT_FOUND) {
            return request(step.call, key, step.call.text, *output);
        }
        if (status != AYNI_OK) {
            return status;
        }
        const auto end = outcome(call);
        if (end == AYNI_RESULT_UNKNOWN || end == AYNI_BUSY) {
            return end;
        }
        if (end != AYNI_OK) {
            return report(*snapshot, *output, AYNI_POLICY_REPORT_FAILURE);
        }
        if (call.tool_call_count) {
            return AYNI_UNSUPPORTED;
        }
    }
    return report(*snapshot, *output, AYNI_POLICY_REPORT_COMPLETION);
}
} // namespace ayni::strategies
