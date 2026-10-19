#include "work/goal/goal_table.h"

#include <cstring>
#include <limits>

namespace ayni::work {
namespace {

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();

} // namespace

std::size_t GoalTable::storage_size(uint32_t capacity, uint32_t parameter_max_bytes) {
    return foundation::BoundedArray<Goal>::storage_size(capacity) +
           static_cast<std::size_t>(capacity) * parameter_max_bytes;
}

GoalTable::GoalTable(void *storage, const Limits &limits)
    : goals_(storage, limits.capacity),
      parameter_blocks_(static_cast<uint8_t *>(storage) +
                        foundation::BoundedArray<Goal>::storage_size(limits.capacity)),
      parameter_max_bytes_(limits.parameter_max_bytes), depth_max_(limits.depth_max),
      children_max_(limits.children_max) {}

GoalTable::Admission GoalTable::admit(const NewGoal &goal) const {
    if (goal.parent_goal_id != 0U) {
        uint32_t parent_index = 0;
        if (!find(goal.parent_goal_id, parent_index)) {
            return Admission::parent_unknown;
        }
        const Goal &parent = goals_[parent_index];
        // A PROPOSED goal has no executable work yet, so it has no children.
        if (parent.lifecycle != AYNI_GOAL_ACTIVE && parent.lifecycle != AYNI_GOAL_PAUSED) {
            return Admission::parent_state;
        }
        if (parent.owner_instance_id != goal.owner_instance_id) {
            return Admission::parent_owner;
        }
        if (parent.depth >= depth_max_) {
            return Admission::too_deep;
        }
        if (live_children(parent_index) >= children_max_) {
            return Admission::too_many_children;
        }
    }
    return goals_.full() ? Admission::full : Admission::admitted;
}

uint32_t GoalTable::insert(const NewGoal &goal, uint64_t goal_id,
                           foundation::TransitionStamp stamp) {
    Goal created{};
    created.goal_id = goal_id;
    created.parent_goal_id = goal.parent_goal_id;
    created.owner_instance_id = goal.owner_instance_id;
    created.created_transition_id = stamp.transition_id;
    created.commit_seq = stamp.commit_seq;
    created.origin = goal.origin;
    created.parent_index = no_parent;
    created.goal_type_index = goal.goal_type_index;
    created.revision = 1;
    created.parameter_size = goal.parameters.size;
    created.lifecycle = AYNI_GOAL_ACTIVE;
    if (goal.parent_goal_id != 0U && find(goal.parent_goal_id, created.parent_index)) {
        created.depth = goals_[created.parent_index].depth + 1U;
    }

    const uint32_t index = goals_.size();
    // Cannot fail: admit() found room.
    static_cast<void>(goals_.push_back(created));
    if (goal.parameters.size > 0U) {
        std::memcpy(parameter_blocks_ + static_cast<std::size_t>(index) * parameter_max_bytes_,
                    goal.parameters.data, goal.parameters.size);
    }
    return index;
}

GoalTable::ChangeCheck GoalTable::check(uint32_t index, GoalChange change,
                                        uint32_t expected_revision) const {
    const Goal &goal = goals_[index];
    if (expected_revision != goal.revision) {
        return ChangeCheck::revision_conflict;
    }
    if (!allows(goal, change)) {
        return ChangeCheck::invalid_state;
    }
    if (goal.revision == max_revision) {
        return ChangeCheck::revision_exhausted;
    }
    if (change == GoalChange::cancel) {
        for (uint32_t later = index + 1U; later < goals_.size(); ++later) {
            const Goal &other = goals_[later];
            if (is_live(other.lifecycle) && other.revision == max_revision &&
                descends_from(later, index)) {
                return ChangeCheck::revision_exhausted;
            }
        }
    }
    return ChangeCheck::allowed;
}

void GoalTable::apply(uint32_t index, GoalChange change, foundation::TransitionStamp stamp) {
    Goal &goal = goals_[index];
    switch (change) {
    case GoalChange::pause:
        set_lifecycle(goal, AYNI_GOAL_PAUSED, stamp);
        return;
    case GoalChange::resume:
        set_lifecycle(goal, AYNI_GOAL_ACTIVE, stamp);
        return;
    case GoalChange::cancel:
        break;
    }
    set_lifecycle(goal, AYNI_GOAL_CANCELLED, stamp);
    // Descendants come after their ancestor.
    for (uint32_t later = index + 1U; later < goals_.size(); ++later) {
        Goal &other = goals_[later];
        if (is_live(other.lifecycle) && descends_from(later, index)) {
            set_lifecycle(other, AYNI_GOAL_CANCELLED, stamp);
        }
    }
}

bool GoalTable::find(uint64_t goal_id, uint32_t &out_index) const {
    for (uint32_t index = 0; index < goals_.size(); ++index) {
        if (goals_[index].goal_id == goal_id) {
            out_index = index;
            return true;
        }
    }
    return false;
}

ayni_bytes_v1 GoalTable::parameters(uint32_t index) const {
    return {parameter_blocks_ + static_cast<std::size_t>(index) * parameter_max_bytes_,
            goals_[index].parameter_size};
}

bool GoalTable::is_live(ayni_goal_lifecycle_t lifecycle) {
    return lifecycle == AYNI_GOAL_PROPOSED || lifecycle == AYNI_GOAL_ACTIVE ||
           lifecycle == AYNI_GOAL_PAUSED;
}

uint32_t GoalTable::live_children(uint32_t parent_index) const {
    uint32_t count = 0;
    for (uint32_t index = parent_index + 1U; index < goals_.size(); ++index) {
        if (goals_[index].parent_index == parent_index && is_live(goals_[index].lifecycle)) {
            ++count;
        }
    }
    return count;
}

// Walks up at most depth_max parents.
bool GoalTable::descends_from(uint32_t index, uint32_t ancestor) const {
    const uint32_t ancestor_depth = goals_[ancestor].depth;
    uint32_t current = index;
    while (goals_[current].depth > ancestor_depth) {
        current = goals_[current].parent_index;
    }
    return current == ancestor;
}

bool GoalTable::allows(const Goal &goal, GoalChange change) {
    switch (change) {
    case GoalChange::pause:
        return goal.lifecycle == AYNI_GOAL_ACTIVE;
    case GoalChange::resume:
        return goal.lifecycle == AYNI_GOAL_PAUSED;
    case GoalChange::cancel:
        return is_live(goal.lifecycle);
    }
    return false;
}

void GoalTable::set_lifecycle(Goal &goal, ayni_goal_lifecycle_t lifecycle,
                              foundation::TransitionStamp stamp) {
    goal.lifecycle = lifecycle;
    ++goal.revision;
    goal.commit_seq = stamp.commit_seq;
    if (lifecycle == AYNI_GOAL_CANCELLED) {
        goal.cancel_revision = goal.revision;
    }
}

} // namespace ayni::work
