// Goals (contracts/domain/lifecycles.md, primitives.md). The table owns each
// goal's lifecycle, revision, place in the goal tree, parameter block,
// trigger cursor and expiry (ADR-0018, ADR-0019, ADR-0020), and records the
// local inputs its triggers take (ADR-0021). It enforces the fixed owner
// (C-LIFE-020), the tree limits (C-LIMIT-007) and the cascades of cancel
// (C-LIFE-022), expiry and a goal's end (ADR-0022), so a goal that is not
// terminal never has a terminal ancestor.
//
// A terminal goal keeps its place until a new goal needs one: then the goal
// that ended first gives way, and a goal that is not terminal never does
// (ADR-0025). A descendant never ends after its ancestor, and of the goals
// that ended in one transition the deepest gives way first, so a goal's
// parent stays in the table as long as the goal does. The new goal takes
// the place given up and no other goal moves, so an index stays valid while
// its goal is in the table; the places are not in creation order, goal ids
// are.
//
// Recovery rebuilds the table from the log (ADR-0026): restore() and
// restore_state() replay what each committed transition changed, and
// resolve_types() and reanchor() then fit the goals to this boot.

#ifndef AYNI_CORE_WORK_GOAL_GOAL_TABLE_H
#define AYNI_CORE_WORK_GOAL_GOAL_TABLE_H

#include "catalog/catalog_id.h"
#include "catalog/goal_type/work_profile.h"
#include "foundation/bounded_array.h"
#include "foundation/transition_stamp.h"
#include "work/authority/authority_facts.h"
#include "work/goal/trigger_schedule.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::work {

class GoalAuthorityTable; // Owns the independent authorization phase.

struct GoalOrigin {
    ayni_origin_t kind;
    ayni_channel_t channel;
    uint64_t principal_id;
    uint32_t binding_revision;
};

// A goal the kernel has resolved and wants to create.
struct NewGoal {
    uint64_t owner_instance_id; // an existing instance
    uint64_t parent_goal_id;    // 0: a top-level goal
    uint32_t goal_type_index;   // in the goal type registry
    GoalOrigin origin;
    ayni_bytes_v1 parameters; // fits parameters_fit()
    Trigger trigger;          // trigger.fits()
    // LOCAL_INPUT: the local input whose events are its occurrences; empty
    // for other kinds.
    catalog::CatalogId trigger_source;
    uint64_t created_us; // on the host monotonic clock
    uint64_t expiry_us;  // from created_us to the expiry; 0: none
    // The goal type's: how long a due occurrence may wait for a task;
    // no_time: unbounded (ADR-0020).
    uint64_t occurrence_wait_max_us;
    // The goal type's id and version, which a later boot resolves again.
    catalog::CatalogId goal_type_id;
    uint32_t goal_type_version;
    catalog::WorkProfile work_profile{}; // Copied trusted classification and background floor.
};

struct Goal {
    uint64_t goal_id;
    uint64_t parent_goal_id;
    uint64_t owner_instance_id;
    uint64_t replaced_goal_id; // the terminal goal whose place it took; 0: none
    foundation::TransitionStamp
        created;           // Immutable creation pair; zero before insert, positive after.
    uint64_t commit_seq;   // of the last change
    uint64_t last_task_id; // 0 before the first task
    Trigger trigger;
    TriggerCursor cursor; // no next occurrence, none waiting, once the goal is terminal
    catalog::CatalogId trigger_source;
    uint64_t expires_us;      // no_time: never
    uint64_t expiry_after_us; // from creation, or from recovery (ADR-0026); 0: none
    uint64_t occurrence_wait_max_us;
    GoalOrigin origin;
    catalog::CatalogId goal_type_id;
    uint32_t goal_type_version;
    uint32_t parent_index; // GoalTable::no_parent for a top-level goal
    // In the goal type registry; GoalTable::no_index when this boot has no
    // goal type of goal_type_id at goal_type_version (resolve_types()).
    uint32_t goal_type_index;
    uint32_t revision;        // 1 at creation, raised by each change
    uint32_t cancel_revision; // the revision that cancelled it; 0 otherwise
    uint32_t depth;
    uint32_t parameter_size;
    uint32_t occurrences_skipped;
    ayni_reason_t skip_reason; // of the latest skip; AYNI_REASON_NONE before any
    // Why this boot holds the goal that is not terminal (ADR-0026):
    // A missing/incompatible type; NONE while compatible. In memory only.
    // Independent authorization phase and binding belong to GoalAuthorityTable.
    ayni_reason_t hold_reason;
    ayni_goal_lifecycle_t lifecycle;
    catalog::WorkProfile
        work_profile{}; // Immutable creation semantics, never inferred from a name.
};

// What a GOAL_STATE record (ADR-0026) holds. The times are this boot's, so
// the record says only whether there are any.
struct GoalState {
    uint64_t last_task_id;
    ayni_goal_lifecycle_t lifecycle;
    uint32_t revision;
    uint32_t cancel_revision;
    uint32_t occurrences_skipped;
    ayni_reason_t skip_reason;
    uint32_t taken;   // TriggerCursor::taken
    uint32_t waiting; // TriggerCursor::waiting
    bool next_scheduled;
    bool expiry_pending;
};

// Expire comes from the kernel at expires_us, never from an intent; satisfy
// and fail come from the kernel when the goal's last task ends by its
// strategy's decision (ADR-0022), and fail also when its last occurrence
// is skipped with no task left (ADR-0023).
enum class GoalChange { pause, resume, cancel, expire, satisfy, fail };

class GoalTable {
  public:
    static constexpr uint32_t no_index = 0xFFFFFFFFU; // no goal's index
    static constexpr uint32_t no_parent = no_index;   // parent_index of a top-level goal

    struct Limits {
        uint32_t capacity;            // at most AYNI_GOAL_CAPACITY_MAX
        uint32_t parameter_max_bytes; // at most AYNI_GOAL_PARAMETER_MAX_BYTES_MAX
        uint32_t depth_max;           // at most AYNI_GOAL_DEPTH_MAX_MAX
        uint32_t children_max;
    };

    // Why a new goal is refused, in the order admit() checks.
    enum class Admission {
        admitted,
        parent_unknown,
        parent_state, // the parent is neither ACTIVE nor PAUSED
        parent_owner, // the parent has another owner
        too_deep,
        too_many_children,
        full,
    };

    // Why a change is refused, in the order check() checks.
    enum class ChangeCheck {
        allowed,
        revision_conflict,
        invalid_state,
        revision_exhausted, // a goal it changes is at the largest revision
    };

    // How a record replayed, as TaskTable::Restore.
    enum class Restore { restored, corrupt, no_room };

    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<Goal>::storage_alignment;
    // The goals, then one parameter block of parameter_max_bytes per goal.
    static std::size_t storage_size(uint32_t capacity, uint32_t parameter_max_bytes);

    // storage holds storage_size(limits.capacity, limits.parameter_max_bytes)
    // bytes aligned to storage_alignment and outlives the table.
    GoalTable(void *storage, const Limits &limits);

    GoalTable(const GoalTable &) = delete;
    GoalTable &operator=(const GoalTable &) = delete;

    // Relation owner decides whether terminal work still depends on this Goal.
    struct Retention {
        const void *context = nullptr; // Borrowed coordinator, outlives this table.
        bool (*retains)(const void *,
                        const Goal &) = nullptr; // Pure owner-thread query; null disables.
    };
    void set_retention(Retention retention) { retention_ = retention; } // Before live work starts.

    bool parameters_fit(uint32_t size) const { return size <= parameter_max_bytes_; }

    Admission admit(const NewGoal &goal) const;

    // The place the next insert() takes from a goal that gives way: when the
    // table is full, that of the terminal goal that ended first, by
    // commit_seq, then the deepest, then the first created (ADR-0025).
    // no_index when the table is not full or no goal is terminal. A goal
    // gives way with its Tasks. The coordinator's retention query protects
    // RUNNING/CANCELLING Tasks and terminal Tasks with unresolved call facts.
    uint32_t giving_way() const;

    // Where insert() put a new goal.
    enum class Placement {
        refused,   // no room: nothing changed
        new_place, // a place no goal held
        given_way, // the place giving_way() named, whose goal is gone
    };

    // Creates an ACTIVE goal at revision 1, copies its parameters and sets
    // out_index. Only after admit(goal) returned admitted, with no change in
    // between; refuses, changing nothing, if the table has no room anyway.
    // The tasks of a goal that gave way still name out_index, so the caller
    // removes them (work::insert_goal()).
    Placement insert(const NewGoal &goal, uint64_t goal_id, foundation::TransitionStamp stamp,
                     uint32_t &out_index);

    // Newly inserted, same transition: withhold all triggers until approval.
    void propose(uint32_t index);
    // Only PROPOSED, unheld, unexpired work can be approved at its exact revision.
    ChangeCheck check_approval(uint32_t index, uint32_t expected_revision, uint64_t now_us) const;
    // Reauthorization requires a retained unexpired active/paused Goal and
    // room for its revision; compatibility holds remain independent.
    ChangeCheck check_reauthorization(uint32_t index, uint32_t expected_revision,
                                      uint64_t now_us) const;
    // After successful whole-batch admission, start the original trigger now;
    // the original expiry remains unchanged. No callbacks may intervene.
    void approve(uint32_t index, uint64_t now_us, foundation::TransitionStamp stamp);
    // Admitted reauthorization preserves user state and the same-boot schedule;
    // only overdue recurring backlog is skipped, using the existing cursor owner.
    void reauthorize(uint32_t index, uint64_t now_us, foundation::TransitionStamp stamp);
    // Authorization changed autonomously; preserve user lifecycle and revision,
    // like trigger progress, but journal this Goal state with the metadata.
    void authority_changed(uint32_t index, foundation::TransitionStamp stamp);
    // Existing live Goal, exact user/trigger state; only its authority stamp advances.
    Restore restore_authority_state(uint32_t index, const GoalState &state,
                                    foundation::TransitionStamp stamp);
    // Exact user state and one revision advance; only recurring un-derived
    // occurrences may become skipped. Replay reanchors times after the batch.
    Restore restore_reauthorization_state(uint32_t index, const GoalState &state,
                                          foundation::TransitionStamp stamp);

    // Pause takes ACTIVE to PAUSED and resume takes it back; neither touches
    // the children. Cancel takes a goal that is not terminal, and every
    // descendant that is not terminal, to CANCELLED; expire likewise to
    // EXPIRED. Satisfy and fail take an ACTIVE goal to SATISFIED and FAILED
    // and every descendant that is not terminal to CANCELLED.
    ChangeCheck check(uint32_t index, GoalChange change, uint32_t expected_revision) const;
    // Only after check() returned allowed, with no change in between. Each
    // goal it changes gets the next revision and stamp.commit_seq.
    void apply(uint32_t index, GoalChange change, foundation::TransitionStamp stamp);

    // Takes the occurrences of goal index due by now_us for task_id and
    // gives the goal stamp.commit_seq. The revision stays: it is what intents
    // name the goal by, and the kernel's own progress through the trigger
    // must not make a caller's pause or cancel stale (ADR-0018). Says nothing
    // about whether the goal may advance; only when
    // at(index).cursor.due_by(now_us).
    TakenOccurrences take_due(uint32_t index, uint64_t now_us, uint64_t task_id,
                              foundation::TransitionStamp stamp);
    // Takes the occurrences of goal index due by now_us without a task,
    // counts them as skipped for reason and gives the goal stamp.commit_seq;
    // the revision stays, as for take_due. Same precondition as take_due.
    void skip_due(uint32_t index, uint64_t now_us, ayni_reason_t reason,
                  foundation::TransitionStamp stamp);
    // Explicit authorization resume skips all unmaterialized interval/input
    // backlog. Once/delay retain their original due time (ADR-0038 decision 11).
    // Caller has already rejected an expired Goal; user pause is unchanged.
    void skip_authority_backlog(uint32_t index, uint64_t now_us, foundation::TransitionStamp stamp);
    // Whether some goal takes a local input from source: its trigger is
    // LOCAL_INPUT from that source, it is not terminal, paused or not, and
    // one more occurrence has a number (ADR-0021).
    bool takes_input(const catalog::CatalogId &source,
                     const GoalAuthorityTable *authority = nullptr,
                     InputResumeBoundary input = {}) const;
    // Records the input as one occurrence, due at now_us, of every goal that
    // takes it, and gives each of them stamp.commit_seq; the revision stays,
    // as for take_due. Only after takes_input(source) returned true.
    void record_input(const catalog::CatalogId &source, uint64_t now_us,
                      foundation::TransitionStamp stamp,
                      const GoalAuthorityTable *authority = nullptr,
                      InputResumeBoundary input = {});
    // Drops the expiry of goal index, which then never expires, and gives it
    // stamp.commit_seq; the revision stays. Only when expiring it would raise
    // a revision past the largest, its own or one of the goals or tasks the
    // expiry reaches (C-COMMIT-012): nothing lowers a revision, so it never
    // could.
    void drop_expiry(uint32_t index, foundation::TransitionStamp stamp);

    // Whether some goal of the instance that is not terminal is at the
    // largest revision.
    bool owned_at_largest_revision(uint64_t instance_id) const;
    // Cancels every goal of the instance that is not terminal, as cancel
    // does; their descendants have the same owner.
    void cancel_owned(uint64_t instance_id, foundation::TransitionStamp stamp);

    // Replays a GOAL record: the goal as insert() created it in stamp, in the
    // place of replaced_goal_id, a terminal goal, or in a new place when
    // that is 0. goal.goal_type_index is ignored until resolve_types(), and
    // the times until reanchor(). The tasks of a goal that gave way still
    // name out_index, so the caller removes them.
    Restore restore(const NewGoal &goal, uint64_t goal_id, uint64_t replaced_goal_id,
                    foundation::TransitionStamp stamp, uint32_t &out_index);
    // Replays a GOAL_STATE record of goal index.
    Restore restore_state(uint32_t index, const GoalState &state,
                          foundation::TransitionStamp stamp);
    // Static snapshots contain terminal parents and retained objects whose
    // replaced predecessors have already gone. Parents precede children.
    Restore restore_snapshot(const NewGoal &goal, uint64_t goal_id, uint64_t replaced_goal_id,
                             uint32_t depth, const GoalState &state,
                             foundation::TransitionStamp created_stamp, uint64_t commit_seq,
                             uint32_t &out_index);
    // Sets each goal's goal_type_index from its type's id and version
    // (ADR-0026): resolve(id, version, trigger_kind, profile, out_index) says whether
    // this boot has that type, and can serve a goal of that trigger kind,
    // as AYNI_REASON_NONE, else why not; a goal that is not terminal and
    // lacks it is held for that reason.
    template <typename Resolve> void resolve_types(Resolve resolve) {
        for (uint32_t index = 0; index < goals_.size(); ++index) {
            Goal &goal = goals_[index];
            goal.goal_type_index = no_index;
            const ayni_reason_t lacks =
                resolve(goal.goal_type_id, goal.goal_type_version, goal.trigger.kind,
                        goal.work_profile, goal.goal_type_index);
            goal.hold_reason = is_live(goal.lifecycle) ? lacks : AYNI_REASON_NONE;
        }
    }
    // Puts the goals' next occurrences and expiries, which a replay leaves
    // undetermined, on this boot's clock at now_us, the time recovery ended
    // (ADR-0026): each wait starts again in full.
    void reanchor(uint64_t now_us);
    // A recovery restriction never rewrites the stored lifecycle or stamps,
    // and never hides a more specific compatibility failure.
    void hold_restored(uint32_t index, ayni_reason_t reason) {
        Goal &goal = goals_[index];
        if (is_live(goal.lifecycle) && goal.hold_reason == AYNI_REASON_NONE) {
            goal.hold_reason = reason;
        }
    }
    // Empties the table, as a recovery that failed leaves it.
    void clear() { goals_.clear(); }

    // Whether this boot holds the goal: it has no due work and takes no
    // input (ADR-0026). Its times stay as recovery set them, so a GOAL_STATE
    // written for it, by a pause say, keeps what the log had pending.
    static bool is_held(const Goal &goal) { return goal.hold_reason != AYNI_REASON_NONE; }

    // Returns false when no goal has the id.
    bool find(uint64_t goal_id, uint32_t &out_index) const;
    // Largest retained stable ID, or 0 for an empty table; a scan frontier.
    uint64_t latest_id() const;
    // Least retained ID in (after, through]; false preserves out_index.
    bool next_in_range(uint64_t after, uint64_t through, uint32_t &out_index) const;
    // Visits start and its ancestors, nearest first. start is a valid index
    // or no_parent (an empty lineage). False stops the walk and is returned;
    // true means all were visited. The visitor must not mutate tree topology.
    template <typename Visit> bool visit_lineage(uint32_t start, Visit visit) const {
        for (auto index = start; index != no_parent; index = goals_[index].parent_index) {
            if (!visit(index)) {
                return false;
            }
        }
        return true;
    }
    // Whether goal index is root or one of its descendants.
    bool in_subtree(uint32_t index, uint32_t root) const;
    static bool at_largest_revision(const Goal &goal);

    // index < size().
    const Goal &at(uint32_t index) const { return goals_[index]; }
    ayni_bytes_v1 parameters(uint32_t index) const;
    // Privacy owner has persisted a separate erasure fact; preserve workflow identity/outcome.
    void erase_parameters(uint32_t index);
    uint32_t size() const { return goals_.size(); }

    // Not terminal: PROPOSED, ACTIVE or PAUSED.
    static bool is_live(ayni_goal_lifecycle_t lifecycle);

  private:
    uint32_t live_children(uint32_t parent_index) const;
    // Whether goal index is a descendant of ancestor, not ancestor itself.
    bool descends_from(uint32_t index, uint32_t ancestor) const;
    // Whether goal gives way before other, both terminal.
    static bool gives_way_before(const Goal &goal, const Goal &other);
    static bool allows(const Goal &goal, GoalChange change);
    // Whether the change makes the goal terminal and so reaches its
    // descendants.
    static bool ends(GoalChange change);
    bool takes(uint32_t index, const catalog::CatalogId &source,
               const GoalAuthorityTable *authority, InputResumeBoundary input) const;
    void set_lifecycle(Goal &goal, ayni_goal_lifecycle_t lifecycle,
                       foundation::TransitionStamp stamp);
    // The goal insert() and restore() create.
    Goal created_goal(const NewGoal &goal, uint64_t goal_id, uint64_t replaced_goal_id,
                      foundation::TransitionStamp stamp) const;
    // Puts created at index, a new place when index is size(), with its
    // parameters.
    void place(const Goal &created, uint32_t index, ayni_bytes_v1 parameters);

    foundation::BoundedArray<Goal> goals_;
    uint8_t *parameter_blocks_;
    uint32_t parameter_max_bytes_;
    Retention retention_{}; // References stay with their owners; no cached dependency count.
    uint32_t depth_max_;
    uint32_t children_max_;
};

} // namespace ayni::work

#endif // AYNI_CORE_WORK_GOAL_GOAL_TABLE_H
