#include "policy_support.h"
namespace ayni::applications::assistant {
ayni_status_t prepare_goal(const Parameters &p, uint64_t owner, uint64_t period,
                           const ayni_calendar_rule_v1 *calendar, Encoded &encoded,
                           ayni_goal_create_v1 &out) {
    auto status = encode(p, encoded);
    if (status != AYNI_OK || !owner) {
        return status == AYNI_OK ? AYNI_INVALID_ARGUMENT : status;
    }
    ayni_goal_create_v1 goal{};
    goal.struct_size = sizeof(goal);
    goal.owner_instance_id = owner;
    goal.goal_type_version = 1;
    goal.goal_type_id = detail::text(goal_type(p.kind));
    goal.parameters = encoded.view();
    if (p.kind == GoalKind::reminder) {
        if (!calendar || calendar->kind < AYNI_CALENDAR_ONCE ||
            calendar->kind > AYNI_CALENDAR_WEEKLY) {
            return AYNI_INVALID_ARGUMENT;
        }
        goal.calendar = calendar;
        goal.trigger = calendar->kind == AYNI_CALENDAR_ONCE    ? AYNI_TRIGGER_CALENDAR_ONCE
                       : calendar->kind == AYNI_CALENDAR_DAILY ? AYNI_TRIGGER_CALENDAR_DAILY
                                                               : AYNI_TRIGGER_CALENDAR_WEEKLY;
    } else {
        goal.pause_group = autonomous_group;
        goal.trigger =
            p.kind == GoalKind::scene_observe ? AYNI_TRIGGER_ONCE : AYNI_TRIGGER_INTERVAL;
        if (goal.trigger == AYNI_TRIGGER_INTERVAL) {
            if (period < check_period_us || period == UINT64_MAX) {
                return AYNI_INVALID_ARGUMENT;
            }
            goal.trigger_period_us = period;
        }
    }
    out = goal;
    return AYNI_OK;
}
ayni_status_t prepare_goal(const Schedule &s, uint64_t owner, Encoded &encoded,
                           ayni_goal_create_v1 &out) {
    return prepare_goal(s.parameters, owner, s.period_us,
                        s.parameters.kind == GoalKind::reminder ? &s.calendar : nullptr, encoded,
                        out);
}
} // namespace ayni::applications::assistant
