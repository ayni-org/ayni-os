#include "kernel/service/intent/pause_group_intent.h"
namespace ayni::kernel {
namespace {
bool member(const WorkModel &model, uint32_t index, uint32_t group) {
    return !model.goals.visit_lineage(
        index, [&](uint32_t at) { return model.goals.at(at).pause_group != group; });
}
// Dropping a suspended periodic cursor is work-owned. Civil time must be trusted
// before discarding calendar backlog, just as for explicit reauthorization.
ayni_status_t preflight_resume(const WorkModel &model, uint32_t group) {
    if (model.calendars.clock().trusted) {
        return AYNI_OK;
    }
    for (uint32_t index = 0; index < model.goals.size(); ++index) {
        const auto &goal = model.goals.at(index);
        if (work::GoalTable::is_live(goal.lifecycle) && goal.lifecycle != AYNI_GOAL_PROPOSED &&
            member(model, index, group) &&
            (goal.trigger.kind == AYNI_TRIGGER_CALENDAR_DAILY ||
             goal.trigger.kind == AYNI_TRIGGER_CALENDAR_WEEKLY)) {
            return AYNI_NOT_READY;
        }
    }
    return AYNI_OK;
}
// Group reopening drops only unmaterialized autonomous backlog. Existing Tasks,
// checkpoints, user pauses and authority holds remain independently owned.
void skip_backlog(WorkModel &model, uint32_t group, uint64_t now_us,
                  foundation::TransitionStamp stamp) {
    for (uint32_t index = 0; index < model.goals.size(); ++index) {
        const auto &goal = model.goals.at(index);
        if (!work::GoalTable::is_live(goal.lifecycle) || goal.lifecycle == AYNI_GOAL_PROPOSED ||
            !member(model, index, group)) {
            continue;
        }
        model.goals.skip_suspended_backlog(index, now_us, AYNI_REASON_PAUSE_GROUP, stamp);
        if ((goal.trigger.kind == AYNI_TRIGGER_CALENDAR_DAILY ||
             goal.trigger.kind == AYNI_TRIGGER_CALENDAR_WEEKLY)) {
            const auto due = model.calendars.due(index);
            if (due.count != 0) {
                static_cast<void>(model.calendars.consume(index, stamp));
                model.goals.skip_calendar(index, due.count, now_us, AYNI_REASON_PAUSE_GROUP, stamp);
            }
        }
    }
}
} // namespace
// All fallible checks precede the single transition. Settlement uses the same
// predicted gate as its preflight, preserving every Goal's original lifecycle.
ayni_status_t set_pause_group(WorkModel &model, const work::PauseGroupChange &change,
                              uint64_t now_us, InputResumeSampler input, IntentOutcome &outcome,
                              IntentFailure &failure) {
    const auto status = model.pause_groups.check(change);
    if (status != AYNI_OK) {
        return status;
    }
    const auto &previous = model.pause_groups.at(change.group);
    const bool reopening = previous.paused && !change.paused;
    const auto resume_status = reopening ? preflight_resume(model, change.group) : AYNI_OK;
    if (resume_status != AYNI_OK) {
        failure.reason = AYNI_REASON_TIME_UNTRUSTED;
        return resume_status;
    }
    auto tables = model.tables();
    if (work::pause_group_change_exhausts(tables, change.group, change.paused)) {
        failure.reason = AYNI_REASON_REVISION_EXHAUSTED;
        return AYNI_CAPACITY_EXCEEDED;
    }
    uint64_t id = previous.object_id;
    if (id == 0 && !model.ids.available()) {
        failure.reason = AYNI_REASON_ID_EXHAUSTED;
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto boundary = reopening ? input.sample() : work::InputResumeBoundary{};
    if (id == 0) {
        static_cast<void>(model.ids.allocate(id));
    }
    const auto stamp = model.transitions.next();
    if (reopening) {
        skip_backlog(model, change.group, now_us, stamp);
    }
    work::apply_pause_group_change(tables, change.group, change.paused, stamp);
    model.pause_groups.apply(change, id, stamp, boundary);
    outcome.object = {0, id, change.expected_revision + 1U, AYNI_OBJECT_PAUSE_GROUP};
    outcome.commit_seq = stamp.commit_seq;
    outcome.due_work = true;
    return AYNI_OK;
}
} // namespace ayni::kernel
