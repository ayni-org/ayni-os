// Work-owned bounded authorization scan (ADR-0038 decision 8). Grant-tree
// cleanup remains in authority; this cursor names stable Goal identities.
#ifndef AYNI_CORE_WORK_AUTHORITY_AUTHORITY_CLEANUP_H
#define AYNI_CORE_WORK_AUTHORITY_AUTHORITY_CLEANUP_H

#include "foundation/transition_stamp.h"

#include <cstdint>

namespace ayni::work {

// Explicit combinations for a scan, independent of any Goal lifecycle.
enum class AuthorityScanPhase : uint32_t { idle = 0, scanning = 1 };

// Complete progress fact, shared by the journal and snapshot codec.
struct AuthorityCleanupRecord {
    foundation::TransitionStamp stamp{}; // Last progress/request pair; both zero before use.
    AuthorityScanPhase phase = AuthorityScanPhase::idle; // Idle iff both active cursors are zero.
    uint64_t after_goal_id = 0;     // Inclusive visited stable ID; zero before the first item.
    uint64_t through_goal_id = 0;   // Frozen inclusive scan frontier; nonzero only while scanning.
    uint64_t requested_through = 0; // Next pass frontier; zero means no request during this pass.
};

// Shape validation only; recovery additionally proves the complete transition.
bool valid_authority_cleanup_record(const AuthorityCleanupRecord &record);
// Exact complete progress fact, including its witness stamp; no live-model lookup.
bool same_authority_cleanup_record(const AuthorityCleanupRecord &a,
                                   const AuthorityCleanupRecord &b);

// Owns fixed-through/monotone-after progress and requests arriving mid-scan.
// All calls run on the work owner thread; this class neither walks nor changes Goals.
class AuthorityCleanup {
  public:
    // A domain change requests all retained Goals through this stable ID.
    // Zero acknowledges an empty range without starting a scan. An active
    // frontier is never extended or discarded by such an acknowledgement.
    // Nonzero frontier is a valid Goal ID; stamp is positive and does not
    // precede the retained stamp. One transition may coalesce requests.
    void request(uint64_t through_goal_id, foundation::TransitionStamp stamp);
    // One inspected Goal; caller selected the least retained ID after the
    // cursor, at most through. The last item also completes the pass.
    // Requires scanning, a valid selected ID, and a positive current stamp.
    void visited(uint64_t goal_id, foundation::TransitionStamp stamp);
    // No retained Goal remains in the active range. Starts a pending pass
    // even when its frontier equals the completed pass (no lost requests).
    // Requires scanning and a positive current stamp that does not go backward.
    void finish(foundation::TransitionStamp stamp);
    // Immutable view, including an explicit initial idle fact.
    const AuthorityCleanupRecord &record() const { return state_; }
    // Whether step has bounded scan work, independent of storage admission.
    bool ready() const { return state_.phase == AuthorityScanPhase::scanning; }
    // Recovery supplies a complete shared-validated fact; no runtime policy.
    void restore_validated(const AuthorityCleanupRecord &record) { state_ = record; }
    // Recovery reset after consumption has stopped.
    void clear() { state_ = {}; }

  private:
    AuthorityCleanupRecord state_{}; // Sole owner of the persisted scan and pending frontier.
};

} // namespace ayni::work
#endif // AYNI_CORE_WORK_AUTHORITY_AUTHORITY_CLEANUP_H
