// Shared Grant fact transitions and two-axis stamp order (ADR-0030/0031).
// Live admission and restoration use these exact rules; no Gate or I/O is involved.
#ifndef AYNI_CORE_AUTHORITY_GRANT_GRANT_STATE_H
#define AYNI_CORE_AUTHORITY_GRANT_GRANT_STATE_H
#include "authority/grant/grant_table.h"
namespace ayni::authority::grant_detail {
// Both axes must name an accepted transition; a partially set stamp is invalid.
inline bool stamped(foundation::TransitionStamp stamp) {
    return stamp.transition_id != 0U && stamp.commit_seq != 0U;
}

// Strict historical order requires both ID and commit sequence to advance.
inline bool after(foundation::TransitionStamp newer, foundation::TransitionStamp older) {
    return newer.transition_id > older.transition_id && newer.commit_seq > older.commit_seq;
}

// Equality keeps the two axes paired; neither alone identifies a fact.
inline bool same_stamp(foundation::TransitionStamp left, foundation::TransitionStamp right) {
    return left.transition_id == right.transition_id && left.commit_seq == right.commit_seq;
}

// Apply a checked Goal terminal transition without inventing owner revocation.
inline void end_by_goal(Grant &grant, foundation::TransitionStamp stamp) {
    grant.lifecycle = AYNI_GRANT_GOAL_ENDED;
    grant.revision = grant_terminal_revision;
    grant.stamp = stamp;
}

// Callers already admitted the transition/stamp. Root cleanup metadata is
// independent; changing a descendant never changes the root's terminal order.
inline void revoke_by(Grant &grant, uint64_t root_id, foundation::TransitionStamp stamp) {
    grant.lifecycle = AYNI_GRANT_REVOKED;
    grant.revision = grant_terminal_revision;
    grant.stamp = stamp;
    grant.revoked_by_root_id = root_id;
}

// Non-roots have no scan; a root is complete only at its frozen frontier.
inline bool cleanup_finished(const Grant &grant) {
    return !is_revocation_root(grant.grant_id, grant.revoked_by_root_id) ||
           grant.cleanup_after_grant_id == grant.cleanup_through_grant_id;
}

// Snapshot retained history permits equality or strict two-axis progress.
inline bool at_or_after(foundation::TransitionStamp newer, foundation::TransitionStamp older) {
    return same_stamp(newer, older) || after(newer, older);
}
} // namespace ayni::authority::grant_detail
#endif
