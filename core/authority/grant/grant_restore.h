// Owns historical ordering, record admission and restoration for the Grant module.
// Uses table storage in place, without owning another copy or touching accepted Gates.
#ifndef AYNI_CORE_AUTHORITY_GRANT_GRANT_RESTORE_H
#define AYNI_CORE_AUTHORITY_GRANT_GRANT_RESTORE_H
#include "authority/grant/grant_table.h"
#include <optional>
namespace ayni::authority {
class GrantRestore {
  public:
    using Restore = GrantTable::Restore; // Shared classified restore result, unchanged for callers.
    // Historical active chain at one sequence; current time and Gates are irrelevant.
    static bool active_at(const GrantTable &table, uint32_t index, uint64_t commit_seq);
    // All retained parent, replacement and revocation relationships agree.
    static bool consistent(const GrantTable &table);
    // Replay a creation into its recorded slot after complete validation.
    static Restore restore_create(GrantTable &table, const GrantRecord &record,
                                  foundation::TransitionStamp stamp,
                                  const GrantReclaimExclusions &excluded);
    // Apply the first terminal fact, including ordered descendant cleanup.
    static Restore restore_state(GrantTable &table, const GrantStateRecord &record,
                                 foundation::TransitionStamp stamp);
    // Append a retained snapshot row after its parent, permitting later terminal ancestors.
    static Restore restore_snapshot(GrantTable &table, const GrantSnapshotRecord &record);
    // Apply accepted root control without rechecking its original session/CAS.
    static Restore restore_revocation(GrantTable &table, const GrantRevocationRecord &record,
                                      foundation::TransitionStamp stamp);
    // Restore a cursor after its optional same-batch descendant state.
    static Restore restore_progress(GrantTable &table, const GrantRevocationProgressRecord &record,
                                    foundation::TransitionStamp stamp);

  private:
    // Snapshot may retain ancestors terminated after this creation; append
    // replay requires every ancestor still active at that point in the log.
    enum class AncestorHistory { active, later_terminal, invalid };
    // Snapshots of commands retain commit sequence only. Creation callers also
    // supply a transition ID, preserving their stricter two-axis ordering.
    static AncestorHistory history_at(const GrantTable &table, uint32_t parent_index,
                                      uint64_t commit_seq,
                                      std::optional<uint64_t> transition_id = std::nullopt);
    static bool
    valid_revocation_metadata(const GrantTable &table, uint64_t grant_id, uint64_t parent_grant_id,
                              ayni_grant_lifecycle_t lifecycle, foundation::TransitionStamp stamp,
                              uint64_t revoked_by_root_id, uint64_t cleanup_after,
                              uint64_t cleanup_through, foundation::TransitionStamp cleanup_stamp);
    // Borrowed record state, independent of journal versus snapshot placement.
    // ACTIVE uses the creation stamp/revision; terminal state follows it.
    struct RestoredState {
        foundation::TransitionStamp created; // Nonzero original transition/commit pair.
        foundation::TransitionStamp stamp;   // Latest terminal pair, or created while ACTIVE.
        uint32_t revision;                   // grant_active_revision or grant_terminal_revision.
        ayni_grant_lifecycle_t lifecycle;    // ACTIVE, EXPIRED or REVOKED, never an effective hold.
    };
    static bool valid_restored_fact(const GrantTable &table, const GrantRecord &record,
                                    const RestoredState &state, uint32_t &out_parent);
    static bool fits_restored_fact(const GrantTable &table, const GrantRecord &record);
    static Restore replay_placement(const GrantTable &table, const GrantRecord &record,
                                    foundation::TransitionStamp created,
                                    const GrantReclaimExclusions &excluded, uint32_t &out_index);
    static void install_restored(GrantTable &table, const GrantRecord &record,
                                 const RestoredState &state, uint32_t index);
};
} // namespace ayni::authority
#endif
