// Generic persistent pause gates; applications own the meaning of group numbers.
#ifndef AYNI_CORE_WORK_GOAL_PAUSE_GROUPS_H
#define AYNI_CORE_WORK_GOAL_PAUSE_GROUPS_H
#include "foundation/transition_stamp.h"
#include "work/authority/authority_facts.h"
#include <array>
#include <ayni/ayni.h>
namespace ayni::work {
// Immutable owner command identity; zero revision creates the group's first fact.
struct PauseGroupChange {
    uint32_t group = 0;             // Application group number in [1, AYNI_PAUSE_GROUP_COUNT].
    uint32_t expected_revision = 0; // Exact current revision; zero before first control.
    bool paused = false;            // Desired gate; independent of Goal and Agent lifecycle.
};
// One group's durable preference; a never-controlled group is open with an empty stamp.
struct PauseGroup {
    uint64_t object_id = 0;      // Stable Kernel ID; zero iff revision/stamp are zero.
    uint32_t revision = 0;       // Monotone CAS; zero iff this group has no stored preference.
    bool paused = false;         // False initially; a closed group rejects new work immediately.
    InputResumeBoundary input{}; // Logical reopening frontier; epoch/sequence, never a timestamp.
    foundation::TransitionStamp stamp{}; // Last control; zero iff object_id is zero.
};
// Owns eight fixed bounded gates, without application names or copied Goal membership.
class PauseGroups {
  public:
    // Zero is the explicit no-group value, accepted only for membership.
    static bool valid_membership(uint32_t group) { return group <= AYNI_PAUSE_GROUP_COUNT; }
    // A command always names one actual group.
    static bool valid_group(uint32_t group) { return group != 0 && valid_membership(group); }
    // Borrow the exact group fact; caller has validated its nonzero number.
    const PauseGroup &at(uint32_t group) const { return groups_[group - 1U]; }
    // No group never blocks; other memberships must have passed shape validation.
    bool paused(uint32_t group) const { return group != 0 && at(group).paused; }
    // Valid group already checked at the request boundary; CAS/exhaustion precede mutation.
    ayni_status_t check(const PauseGroupChange &change) const;
    // Publish a preflighted control with its stable object and complete transition stamp.
    void apply(const PauseGroupChange &change, uint64_t object_id,
               foundation::TransitionStamp stamp, InputResumeBoundary input);
    // Intrinsic fact shape, shared by byte and typed input boundaries.
    static bool valid_fact(const PauseGroup &row);
    // Framing already validated; journal requires exactly the next revision.
    bool restore_journal_validated(uint32_t group, const PauseGroup &row);
    // Framing already validated; snapshots restore one retained preference per group.
    bool restore_snapshot_validated(uint32_t group, const PauseGroup &row);
    // Reject both suspended input and queued input older than the explicit reopen.
    bool allows_input(uint32_t group, InputResumeBoundary input) const {
        return group == 0 || (!at(group).paused && !input_precedes_resume(input, at(group).input));
    }
    // Whether no preference has ever been accepted; used before initial owner adoption.
    bool empty() const {
        for (const auto &group : groups_) {
            if (group.object_id != 0) {
                return false;
            }
        }
        return true;
    }
    // Recovery discard keeps application-independent defaults, never stale gates.
    void clear() { groups_ = {}; }

  private:
    static constexpr uint32_t first_revision = 1; // Initial accepted control has no resume frontier.
    // Only a closed-to-open control establishes a new queued-input frontier.
    static bool reopens(const PauseGroup &before, bool paused) { return before.paused && !paused; }
    std::array<PauseGroup, AYNI_PAUSE_GROUP_COUNT> groups_{}; // One slot per public group number.
};
} // namespace ayni::work
#endif
