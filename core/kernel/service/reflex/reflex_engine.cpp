#include "kernel/service/reflex/reflex_engine.h"

#include "foundation/deadline.h"

namespace ayni::kernel {
namespace {

ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

// True when a single observation condition is missing, unusable, non-MEASURED, or past max_age_us.
bool condition_stale(const knowledge::ObservationTable &observations,
                     const catalog::ObservationCondition &cond, uint64_t now_us,
                     int64_t &out_value) {
    ayni_observation_info_v1 info{};
    const auto status = observations.info(view(cond.source), now_us, info);
    if (status != AYNI_OK) {
        out_value = 0;
        return true;
    }
    out_value = info.value;
    return info.usable == 0U || info.origin != AYNI_OBSERVATION_MEASURED ||
           now_us < info.captured_at_us || now_us - info.captured_at_us > cond.max_age_us;
}

} // namespace

// Verifies an optional Scene gate is currently enabled, durable and satisfies its own conditions.
bool ReflexEngine::scene_active(const ReflexRuleEntry &rule, uint64_t now_us,
                                uint64_t durable_seq) const {
    if (rule.scene_index == ReflexRuleEntry::no_index) {
        return true;
    }
    uint32_t definition_index = 0;
    if (work_.scenes.current(view(rule.scene_id), rule.scene_version, durable_seq,
                             definition_index) != AYNI_OK) {
        return false;
    }
    const auto &definition = work_.scenes.definition(definition_index);
    for (uint32_t at = 0; at < definition.condition_count; ++at) {
        ayni_observation_ref_v1 ref{};
        if (observations_.require(definition.conditions[at].predicate, now_us, ref, 0U) !=
            AYNI_OK) {
            return false;
        }
    }
    return true;
}

// Evaluates IN_RANGE (all conditions fresh MEASURED+GOOD within [min, max]) or
// STALE_OR_GAP (any listed source missing/stale/degraded/gapped while EPHEMERAL holds resource).
bool ReflexEngine::evaluate_trigger(const ReflexRuleEntry &rule, uint64_t now_us,
                                    ayni_catalog_name_v1 &out_source, int64_t &out_value) const {
    out_source = {};
    out_value = 0;
    if (rule.trigger_mode == AYNI_REFLEX_TRIGGER_STALE_OR_GAP) {
        if (!calls_.has_ephemeral_holder(rule.resource_index)) {
            return false;
        }
        for (uint32_t at = 0; at < rule.condition_count; ++at) {
            int64_t value = 0;
            if (condition_stale(observations_, rule.conditions[at], now_us, value)) {
                out_source = rule.conditions[at].source.snapshot();
                out_value = value;
                return true;
            }
        }
        return false;
    }
    for (uint32_t at = 0; at < rule.condition_count; ++at) {
        ayni_observation_ref_v1 ref{};
        if (observations_.require(rule.conditions[at], now_us, ref, 0U) != AYNI_OK) {
            return false;
        }
    }
    out_source = rule.conditions[0].source.snapshot();
    ayni_observation_info_v1 info{};
    if (observations_.info(view(rule.conditions[0].source), now_us, info) == AYNI_OK) {
        out_value = info.value;
    }
    return true;
}

// Executes one matched rule action, updates volatile rule counters/latches, and pushes one
// diagnostic event. SOFT_STOP_RESOURCE skips silently when no uncancelled holder occupies target.
ayni_status_t ReflexEngine::execute_rule(uint32_t index, uint64_t now_us, uint64_t durable_seq,
                                         const ayni_catalog_name_v1 &source, int64_t value) {
    auto &rule = table_.entry(index);
    ayni_status_t status = AYNI_OK;
    uint64_t affected = 0;
    uint64_t spawned = 0;
    if (rule.action == AYNI_REFLEX_SOFT_STOP_RESOURCE) {
        status = calls_.preempt_resource(rule.resource_index, now_us, work::CallCancelCause::reflex,
                                         affected);
        if (status == AYNI_NOT_FOUND) {
            return AYNI_OK; // Target resource is idle or already stopping; do not consume cooldown.
        }
        if (status == AYNI_INTERNAL) {
            return AYNI_INTERNAL;
        }
    } else {
        if (rule.active_call_id != 0U) {
            return AYNI_OK; // Previous reflex Call from this rule is still in flight.
        }
        status =
            calls_.submit_reflex_call(rule.owner_agent_id, rule.binding_index, view(rule.rule_id),
                                      table_.input(index), now_us, durable_seq, spawned);
        if (status == AYNI_INTERNAL) {
            return AYNI_INTERNAL;
        }
        if (status == AYNI_OK) {
            rule.active_call_id = spawned;
        }
    }
    if (status == AYNI_OK) {
        ++rule.fire_count;
    } else {
        ++rule.denied_count;
    }
    rule.has_fired = true;
    rule.last_fired_us = now_us;
    if ((rule.flags & AYNI_REFLEX_FLAG_EDGE_TRIGGERED) != 0U) {
        rule.latched_active = true;
    }
    ayni_reflex_event_v1 event{};
    event.fired_at_us = now_us;
    event.rule_id = rule.rule_id.snapshot();
    event.action = rule.action;
    event.status = status;
    event.affected_call_id = affected;
    event.spawned_call_id = spawned;
    event.trigger_source = source;
    event.trigger_value = value;
    table_.push_event(event);
    AuditEntry audit{};
    audit.kind = AYNI_AUDIT_REFLEX_EVENT;
    audit.boot_epoch = work_.ids.boot_epoch();
    audit.at_us = now_us;
    audit.subject_id = spawned != 0U ? spawned : affected;
    audit.auxiliary_id = affected;
    audit.status = status;
    audit.reason = status == AYNI_OK ? AYNI_REASON_REFLEX : AYNI_REASON_NONE;
    audit.detail_code = rule.action;
    audit.flags = AYNI_AUDIT_FLAG_EPHEMERAL |
                  (status == AYNI_OK ? AYNI_AUDIT_FLAG_PREEMPTED : AYNI_AUDIT_FLAG_DENIED);
    audit.label = view(rule.rule_id);
    audit_.emit(audit);
    return AYNI_OK;
}

void ReflexEngine::reap_completed() {
    for (uint32_t pos = 0; pos < table_.count(); ++pos) {
        auto &rule = table_.entry(table_.ordered_index(pos));
        if (rule.active_call_id != 0U && !calls_.reflex_call_active(rule.active_call_id)) {
            rule.active_call_id = 0U;
        }
    }
}

ayni_status_t ReflexEngine::evaluate(uint64_t now_us, uint64_t durable_seq) {
    reap_completed();
    for (uint32_t pos = 0; pos < table_.count(); ++pos) {
        const uint32_t index = table_.ordered_index(pos);
        auto &rule = table_.entry(index);
        if (!rule.enabled || !scene_active(rule, now_us, durable_seq)) {
            rule.latched_active = false;
            continue;
        }
        ayni_catalog_name_v1 source{};
        int64_t value = 0;
        if (!evaluate_trigger(rule, now_us, source, value)) {
            rule.latched_active = false;
            continue;
        }
        if ((rule.flags & AYNI_REFLEX_FLAG_EDGE_TRIGGERED) != 0U && rule.latched_active) {
            continue;
        }
        if (rule.has_fired && rule.cooldown_us != 0U && now_us >= rule.last_fired_us &&
            now_us - rule.last_fired_us < rule.cooldown_us) {
            continue;
        }
        if (execute_rule(index, now_us, durable_seq, source, value) != AYNI_OK) {
            return AYNI_INTERNAL;
        }
    }
    return AYNI_OK;
}

// Schedules owner wakeups for active STALE_OR_GAP fail-safe rules so a dead sensor wakes
// ayni_step() right when its sample crosses max_age_us or valid_until_us (ADR-0112).
uint64_t ReflexEngine::next_deadline_us(uint64_t now_us, uint64_t durable_seq) const {
    uint64_t next = UINT64_MAX;
    for (uint32_t pos = 0; pos < table_.count(); ++pos) {
        const auto &rule = table_.entry(table_.ordered_index(pos));
        if (!rule.enabled || rule.trigger_mode != AYNI_REFLEX_TRIGGER_STALE_OR_GAP ||
            !scene_active(rule, now_us, durable_seq) ||
            !calls_.has_ephemeral_holder(rule.resource_index)) {
            continue;
        }
        for (uint32_t at = 0; at < rule.condition_count; ++at) {
            const auto &cond = rule.conditions[at];
            ayni_observation_info_v1 info{};
            if (observations_.info(view(cond.source), now_us, info) != AYNI_OK ||
                info.usable == 0U) {
                return now_us;
            }
            // condition_stale() uses strict age > max_age_us, so staleness starts at +1 us.
            const uint64_t age_limit = foundation::deadline(
                foundation::deadline(info.captured_at_us, cond.max_age_us), 1U);
            const uint64_t stale_at =
                age_limit < info.valid_until_us ? age_limit : info.valid_until_us;
            if (stale_at < next) {
                next = stale_at;
            }
        }
    }
    return next;
}

} // namespace ayni::kernel
