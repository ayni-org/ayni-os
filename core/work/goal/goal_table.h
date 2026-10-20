// Goals (contracts/domain/lifecycles.md, primitives.md). The table owns each
// goal's lifecycle, revision, place in the goal tree, parameter block,
// trigger cursor and expiry (ADR-0018, ADR-0019, ADR-0020), and records the
// local inputs its triggers take (ADR-0021). It enforces the fixed owner
// (C-LIFE-020), the tree limits (C-LIMIT-007) and the cascades of cancel
// (C-LIFE-022), expiry and a goal's end (ADR-0022), so a goal that is not
// terminal never has a terminal ancestor.
//
// Goals are never removed: a terminal goal stays queryable and counts toward
// the capacity until the log defines when it may be dropped (MA-010). So the
// goals stay in creation order, a parent before its children, and an index
// stays valid for the kernel's life.

#ifndef AYNI_CORE_WORK_GOAL_GOAL_TABLE_H
#define AYNI_CORE_WORK_GOAL_GOAL_TABLE_H

#include "catalog/catalog_id.h"
#include "foundation/bounded_array.h"
#include "foundation/transition_stamp.h"
#include "work/goal/trigger_schedule.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::work {

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
};

struct Goal {
    uint64_t goal_id;
    uint64_t parent_goal_id;
    uint64_t owner_instance_id;
    uint64_t created_transition_id;
    uint64_t commit_seq;   // of the last change
    uint64_t last_task_id; // 0 before the first task
    Trigger trigger;
    TriggerCursor cursor; // no next occurrence, none waiting, once the goal is terminal
    catalog::CatalogId trigger_source;
    uint64_t expires_us; // no_time: never
    uint64_t occurrence_wait_max_us;
    GoalOrigin origin;
    uint32_t parent_index; // GoalTable::no_parent for a top-level goal
    uint32_t goal_type_index;
    uint32_t revision;        // 1 at creation, raised by each change
    uint32_t cancel_revision; // the revision that cancelled it; 0 otherwise
    uint32_t depth;
    uint32_t parameter_size;
    uint32_t occurrences_skipped;
    ayni_reason_t skip_reason; // of the latest skip; AYNI_REASON_NONE before any
    ayni_goal_lifecycle_t lifecycle;
};

// Expire comes from the kernel at expires_us, never from an intent; satisfy
// and fail come from the kernel when the goal's last task ends by its
// strategy's decision (ADR-0022).
enum class GoalChange { pause, resume, cancel, expire, satisfy, fail };

class GoalTable {
  public:
    static constexpr uint32_t no_parent = 0xFFFFFFFFU;

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

    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<Goal>::storage_alignment;
    // The goals, then one parameter block of parameter_max_bytes per goal.
    static std::size_t storage_size(uint32_t capacity, uint32_t parameter_max_bytes);

    // storage holds storage_size(limits.capacity, limits.parameter_max_bytes)
    // bytes aligned to storage_alignment and outlives the table.
    GoalTable(void *storage, const Limits &limits);

    GoalTable(const GoalTable &) = delete;
    GoalTable &operator=(const GoalTable &) = delete;

    bool parameters_fit(uint32_t size) const { return size <= parameter_max_bytes_; }

    Admission admit(const NewGoal &goal) const;

    // Creates an ACTIVE goal at revision 1, copies its parameters and sets
    // out_index. Only after admit(goal) returned admitted, with no change in
    // between; returns false, changing nothing, if the table is full anyway.
    bool insert(const NewGoal &goal, uint64_t goal_id, foundation::TransitionStamp stamp,
                uint32_t &out_index);

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
    // Whether some goal takes a local input from source: its trigger is
    // LOCAL_INPUT from that source, it is not terminal, paused or not, and
    // one more occurrence has a number (ADR-0021).
    bool takes_input(const catalog::CatalogId &source) const;
    // Records the input as one occurrence, due at now_us, of every goal that
    // takes it, and gives each of them stamp.commit_seq; the revision stays,
    // as for take_due. Only after takes_input(source) returned true.
    void record_input(const catalog::CatalogId &source, uint64_t now_us,
                      foundation::TransitionStamp stamp);
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

    // Returns false when no goal has the id.
    bool find(uint64_t goal_id, uint32_t &out_index) const;
    // Whether goal index is root or one of its descendants.
    bool in_subtree(uint32_t index, uint32_t root) const;
    static bool at_largest_revision(const Goal &goal);

    // index < size().
    const Goal &at(uint32_t index) const { return goals_[index]; }
    ayni_bytes_v1 parameters(uint32_t index) const;
    uint32_t size() const { return goals_.size(); }

    // Not terminal: PROPOSED, ACTIVE or PAUSED.
    static bool is_live(ayni_goal_lifecycle_t lifecycle);

  private:
    uint32_t live_children(uint32_t parent_index) const;
    // Whether goal index is a descendant of ancestor, which comes before it.
    bool descends_from(uint32_t index, uint32_t ancestor) const;
    static bool allows(const Goal &goal, GoalChange change);
    // Whether the change makes the goal terminal and so reaches its
    // descendants.
    static bool ends(GoalChange change);
    static bool takes(const Goal &goal, const catalog::CatalogId &source);
    void set_lifecycle(Goal &goal, ayni_goal_lifecycle_t lifecycle,
                       foundation::TransitionStamp stamp);

    foundation::BoundedArray<Goal> goals_;
    uint8_t *parameter_blocks_;
    uint32_t parameter_max_bytes_;
    uint32_t depth_max_;
    uint32_t children_max_;
};

} // namespace ayni::work

#endif // AYNI_CORE_WORK_GOAL_GOAL_TABLE_H
