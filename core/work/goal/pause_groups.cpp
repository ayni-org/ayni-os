#include "work/goal/pause_groups.h"
#include "foundation/kernel_id.h"
#include <limits>
namespace ayni::work {
ayni_status_t PauseGroups::check(const PauseGroupChange &change) const {
    const auto &row = at(change.group);
    if (row.revision != change.expected_revision) {
        return AYNI_REVISION_CONFLICT;
    }
    return row.revision == std::numeric_limits<uint32_t>::max() ? AYNI_CAPACITY_EXCEEDED : AYNI_OK;
}
void PauseGroups::apply(const PauseGroupChange &change, uint64_t object_id,
                        foundation::TransitionStamp stamp, InputResumeBoundary input) {
    auto &row = groups_[change.group - 1U];
    // Repeating an open preference must preserve its existing queued-input boundary.
    const auto frontier = reopens(row, change.paused) ? input : row.input;
    row = {object_id, row.revision + 1U, change.paused, frontier, stamp};
}
bool PauseGroups::valid_fact(const PauseGroup &row) {
    return foundation::is_kernel_id(row.object_id) && row.revision != 0 &&
           row.stamp.transition_id != 0 && row.stamp.commit_seq != 0 &&
           valid_input_resume_boundary(row.input) &&
           (row.revision != first_revision || row.input.incarnation == 0);
}
// A group keeps its original ID for life and requires the exact next journal CAS.
bool PauseGroups::restore_journal_validated(uint32_t group, const PauseGroup &row) {
    auto &previous = groups_[group - 1U];
    if (previous.revision == std::numeric_limits<uint32_t>::max() ||
        row.revision != previous.revision + 1U ||
        (previous.object_id != 0 && previous.object_id != row.object_id) ||
        row.stamp.commit_seq <= previous.stamp.commit_seq) {
        return false;
    }
    if (reopens(previous, row.paused)) {
        if (row.input.incarnation == 0 || row.input.incarnation < previous.input.incarnation ||
            (row.input.incarnation == previous.input.incarnation &&
             row.input.through < previous.input.through)) {
            return false;
        }
    } else if (row.input.incarnation != previous.input.incarnation ||
               row.input.through != previous.input.through) {
        return false; // An ordinary preference update cannot erase the accepted input boundary.
    }
    previous = row;
    return true;
}
bool PauseGroups::restore_snapshot_validated(uint32_t group, const PauseGroup &row) {
    auto &previous = groups_[group - 1U];
    if (previous.object_id != 0) {
        return false;
    }
    previous = row;
    return true;
}
} // namespace ayni::work
