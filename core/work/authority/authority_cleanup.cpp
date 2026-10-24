#include "work/authority/authority_cleanup.h"

#include "foundation/kernel_id.h"

namespace ayni::work {

bool valid_authority_cleanup_record(const AuthorityCleanupRecord &record) {
    if ((record.stamp.commit_seq == 0U) != (record.stamp.transition_id == 0U)) {
        return false;
    }
    if (record.phase == AuthorityScanPhase::idle) {
        return record.after_goal_id == 0U && record.through_goal_id == 0U &&
               record.requested_through == 0U;
    }
    return record.phase == AuthorityScanPhase::scanning && record.stamp.commit_seq != 0U &&
           foundation::is_kernel_id(record.through_goal_id) &&
           (record.after_goal_id == 0U || foundation::is_kernel_id(record.after_goal_id)) &&
           record.after_goal_id < record.through_goal_id &&
           (record.requested_through == 0U || foundation::is_kernel_id(record.requested_through));
}

void AuthorityCleanup::request(uint64_t through_goal_id, foundation::TransitionStamp stamp) {
    if (through_goal_id == 0U) {
        // An empty request is still witnessed by its enclosing transition;
        // it creates no scan and never discards an already active frontier.
        state_.stamp = stamp;
        return;
    }
    if (!ready()) {
        state_ = {stamp, AuthorityScanPhase::scanning, 0, through_goal_id, 0};
        return;
    }
    if (through_goal_id > state_.requested_through) {
        state_.requested_through = through_goal_id;
    }
    state_.stamp = stamp;
}

bool same_authority_cleanup_record(const AuthorityCleanupRecord &a,
                                   const AuthorityCleanupRecord &b) {
    return a.stamp.transition_id == b.stamp.transition_id &&
           a.stamp.commit_seq == b.stamp.commit_seq && a.phase == b.phase &&
           a.after_goal_id == b.after_goal_id && a.through_goal_id == b.through_goal_id &&
           a.requested_through == b.requested_through;
}

void AuthorityCleanup::visited(uint64_t goal_id, foundation::TransitionStamp stamp) {
    if (goal_id == state_.through_goal_id) {
        finish(stamp);
        return;
    }
    state_.after_goal_id = goal_id;
    state_.stamp = stamp;
}

void AuthorityCleanup::finish(foundation::TransitionStamp stamp) {
    const auto pending = state_.requested_through;
    state_ = {stamp};
    if (pending != 0U) {
        state_.phase = AuthorityScanPhase::scanning;
        state_.through_goal_id = pending;
    }
}

} // namespace ayni::work
