// Goals (contracts/domain/lifecycles.md, primitives.md). The table owns each
// goal's lifecycle, revision, place in the goal tree and parameter block. It
// enforces the fixed owner (C-LIFE-020), the tree limits (C-LIMIT-007) and
// the cancel cascade (C-LIFE-022).
//
// Goals are never removed: a terminal goal stays queryable and counts toward
// the capacity until the log defines when it may be dropped (MA-010). So the
// goals stay in creation order, a parent before its children, and an index
// stays valid for the kernel's life.

#ifndef AYNI_CORE_WORK_GOAL_GOAL_TABLE_H
#define AYNI_CORE_WORK_GOAL_GOAL_TABLE_H

#include "foundation/bounded_array.h"
#include "foundation/transition_stamp.h"

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
};

struct Goal {
    uint64_t goal_id;
    uint64_t parent_goal_id;
    uint64_t owner_instance_id;
    uint64_t created_transition_id;
    uint64_t commit_seq; // of the last change
    GoalOrigin origin;
    uint32_t parent_index; // GoalTable::no_parent for a top-level goal
    uint32_t goal_type_index;
    uint32_t revision;        // 1 at creation, raised by each change
    uint32_t cancel_revision; // the revision that cancelled it; 0 otherwise
    uint32_t depth;
    uint32_t parameter_size;
    ayni_goal_lifecycle_t lifecycle;
};

enum class GoalChange { pause, resume, cancel };

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

    // Creates an ACTIVE goal at revision 1 and copies its parameters. Only
    // after admit(goal) returned admitted, with no change in between. Returns
    // the new goal's index.
    uint32_t insert(const NewGoal &goal, uint64_t goal_id, foundation::TransitionStamp stamp);

    // Pause takes ACTIVE to PAUSED and resume takes it back; neither touches
    // the children. Cancel takes a goal that is not terminal, and every
    // descendant that is not terminal, to CANCELLED.
    ChangeCheck check(uint32_t index, GoalChange change, uint32_t expected_revision) const;
    // Only after check() returned allowed, with no change in between. Each
    // goal it changes gets the next revision and stamp.commit_seq.
    void apply(uint32_t index, GoalChange change, foundation::TransitionStamp stamp);

    // Returns false when no goal has the id.
    bool find(uint64_t goal_id, uint32_t &out_index) const;

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
