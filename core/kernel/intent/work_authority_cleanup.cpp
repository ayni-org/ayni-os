// Kernel only coordinates the Work-owned scan with Grant consumption checks;
// work retains progress, authority retains Grant state and reclaim fences.
#include "kernel/intent/work_model.h"

namespace ayni::kernel {

// Every terminal dependency/Goal change requests a persisted bounded pass.
// A scan's own progress does not request another pass without a domain change.
void WorkModel::request_authority_cleanup(foundation::TransitionStamp stamp) {
    if (authority_cleanup_change_in(stamp)) {
        authority_cleanup.request(goals.latest_id(), stamp);
    }
}

bool WorkModel::authority_cleanup_change_in(foundation::TransitionStamp stamp) const {
    const auto *mapping = commands.mapped_in(stamp.commit_seq);
    if (mapping != nullptr && (mapping->kind == AYNI_INTENT_GRANT_REVOKE ||
                               mapping->kind == AYNI_INTENT_GOAL_REAUTHORIZE)) {
        // Revoke includes no-change/gone results; a replacement invalidates
        // descendants' parent generations even before their scan (ADR-0038).
        return true;
    }
    for (uint32_t index = 0; index < goals.size(); ++index) {
        const auto &goal = goals.at(index);
        if (goal.commit_seq == stamp.commit_seq && !work::GoalTable::is_live(goal.lifecycle) &&
            grants.has_active_goal_grants(goal.goal_id)) {
            return true;
        }
        if (goal_authority.contains(index, goal.goal_id)) {
            const auto authority = goal_authority.at(index);
            if (authority.phase == work::GoalAuthorityPhase::paused &&
                authority.stamp.commit_seq == stamp.commit_seq) {
                return true;
            }
        }
    }
    for (uint32_t index = 0; index < grants.size(); ++index) {
        const auto &grant = grants.at(index);
        if (grant.stamp.commit_seq == stamp.commit_seq && grant.lifecycle != AYNI_GRANT_ACTIVE) {
            return true;
        }
    }
    return false;
}

// Merge the two owners' stable ranges; an orphan's ID remains visitable after
// Goal slot reuse, so its dedicated Grant cannot evade terminal cleanup.
bool WorkModel::next_authority_cleanup_goal(AuthorityCleanupGoal &out) const {
    if (!authority_cleanup.ready()) {
        return false;
    }
    const auto &progress = authority_cleanup.record();
    AuthorityCleanupGoal selected{};
    uint32_t index = 0;
    if (goals.next_in_range(progress.after_goal_id, progress.through_goal_id, index)) {
        selected = {goals.at(index).goal_id, index};
    }
    const auto bound =
        grants.next_bound_goal_after(progress.after_goal_id, progress.through_goal_id);
    if (bound != 0U && (selected.goal_id == 0U || bound < selected.goal_id)) {
        selected = {bound, work::GoalTable::no_index};
    }
    if (selected.goal_id == 0U) {
        return false;
    }
    out = selected;
    return true;
}

// One selected stable identity is the complete scan item; no descendant walk
// or second transition hides in it. Instant consumption checks remain active.
bool WorkModel::advance_authority_cleanup(uint64_t now_us, uint64_t durable_seq) {
    if (!authority_cleanup.ready()) {
        return false;
    }
    AuthorityCleanupGoal selected{};
    if (!next_authority_cleanup_goal(selected)) {
        authority_cleanup.finish(transitions.next());
        return true;
    }
    const auto index = selected.index;
    const auto before = transitions.next_commit_seq();
    if (index != work::GoalTable::no_index && work::GoalTable::is_live(goals.at(index).lifecycle) &&
        goal_authority.authorized(index, selected.goal_id)) {
        const auto check = check_goal_authority(index, now_us, durable_seq);
        // Catalog compatibility and subject availability remain independent
        // holds (C-AUTH-020/C-STORE-014), never sticky authorization changes.
        if (check.status == AYNI_DENIED && check.reason != AYNI_REASON_GRANT_SUBJECT) {
            pause_goal_authority(index);
        }
    }
    const auto stamp =
        transitions.next_commit_seq() == before ? transitions.next() : transitions.latest();
    if (index == work::GoalTable::no_index ||
        !work::GoalTable::is_live(goals.at(index).lifecycle)) {
        grants.end_goal_grants(selected.goal_id, stamp);
    }
    authority_cleanup.visited(selected.goal_id, stamp);
    return true;
}

} // namespace ayni::kernel
