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
    return goals_.full() && giving_way() == no_index ? Admission::full : Admission::admitted;
}

uint32_t GoalTable::giving_way() const {
    if (!goals_.full()) {
        return no_index;
    }
    uint32_t oldest = no_index;
    for (uint32_t index = 0; index < goals_.size(); ++index) {
        const Goal &ended = goals_[index];
        if (!is_live(ended.lifecycle) &&
            (oldest == no_index || gives_way_before(ended, goals_[oldest]))) {
            oldest = index;
        }
    }
    return oldest;
}

bool GoalTable::insert(const NewGoal &goal, uint64_t goal_id, foundation::TransitionStamp stamp,
                       uint32_t &out_index) {
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
    created.trigger = goal.trigger;
    created.cursor = start_schedule(goal.trigger, goal.created_us);
    created.trigger_source = goal.trigger_source;
    created.expires_us =
        goal.expiry_us == 0U ? no_time : time_after(goal.created_us, goal.expiry_us);
    created.occurrence_wait_max_us = goal.occurrence_wait_max_us;
    created.lifecycle = AYNI_GOAL_ACTIVE;
    if (goal.parent_goal_id != 0U && find(goal.parent_goal_id, created.parent_index)) {
        created.depth = goals_[created.parent_index].depth + 1U;
    }

    // Only a full table gives up a place, and the new goal takes it at once.
    uint32_t index = goals_.size();
    if (goals_.full()) {
        // The parent is ACTIVE or PAUSED (admit()), so it is not giving way.
        index = giving_way();
        if (index == no_index) {
            return false;
        }
        goals_[index] = created;
    } else {
        // Cannot fail: the table is not full.
        static_cast<void>(goals_.push_back(created));
    }
    if (goal.parameters.size > 0U) {
        std::memcpy(parameter_blocks_ + static_cast<std::size_t>(index) * parameter_max_bytes_,
                    goal.parameters.data, goal.parameters.size);
    }
    out_index = index;
    return true;
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
    if (at_largest_revision(goal)) {
        return ChangeCheck::revision_exhausted;
    }
    if (ends(change)) {
        for (uint32_t other = 0; other < goals_.size(); ++other) {
            if (is_live(goals_[other].lifecycle) && at_largest_revision(goals_[other]) &&
                descends_from(other, index)) {
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
    case GoalChange::expire:
    case GoalChange::satisfy:
    case GoalChange::fail:
        break;
    }
    // An expiry reaches the descendants as it is; any other end cancels them.
    ayni_goal_lifecycle_t ended = AYNI_GOAL_CANCELLED;
    ayni_goal_lifecycle_t descendants_end = AYNI_GOAL_CANCELLED;
    if (change == GoalChange::expire) {
        ended = AYNI_GOAL_EXPIRED;
        descendants_end = AYNI_GOAL_EXPIRED;
    } else if (change == GoalChange::satisfy) {
        ended = AYNI_GOAL_SATISFIED;
    } else if (change == GoalChange::fail) {
        ended = AYNI_GOAL_FAILED;
    }
    set_lifecycle(goal, ended, stamp);
    // A descendant may lie before or after its ancestor; a change of one
    // does not change whether another descends from the goal.
    for (uint32_t other = 0; other < goals_.size(); ++other) {
        if (is_live(goals_[other].lifecycle) && descends_from(other, index)) {
            set_lifecycle(goals_[other], descendants_end, stamp);
        }
    }
}

TakenOccurrences GoalTable::take_due(uint32_t index, uint64_t now_us, uint64_t task_id,
                                     foundation::TransitionStamp stamp) {
    Goal &goal = goals_[index];
    const TakenOccurrences taken = work::take_due(goal.trigger, now_us, goal.cursor);
    goal.last_task_id = task_id;
    goal.commit_seq = stamp.commit_seq;
    return taken;
}

void GoalTable::skip_due(uint32_t index, uint64_t now_us, ayni_reason_t reason,
                         foundation::TransitionStamp stamp) {
    Goal &goal = goals_[index];
    // At most every number the goal has, so the count cannot overflow.
    goal.occurrences_skipped += work::take_due(goal.trigger, now_us, goal.cursor).count;
    goal.skip_reason = reason;
    goal.commit_seq = stamp.commit_seq;
}

bool GoalTable::takes_input(const catalog::CatalogId &source) const {
    for (uint32_t index = 0; index < goals_.size(); ++index) {
        if (takes(goals_[index], source)) {
            return true;
        }
    }
    return false;
}

void GoalTable::record_input(const catalog::CatalogId &source, uint64_t now_us,
                             foundation::TransitionStamp stamp) {
    for (uint32_t index = 0; index < goals_.size(); ++index) {
        Goal &goal = goals_[index];
        if (takes(goal, source)) {
            work::record(now_us, goal.cursor);
            goal.commit_seq = stamp.commit_seq;
        }
    }
}

void GoalTable::drop_expiry(uint32_t index, foundation::TransitionStamp stamp) {
    Goal &goal = goals_[index];
    goal.expires_us = no_time;
    goal.commit_seq = stamp.commit_seq;
}

bool GoalTable::owned_at_largest_revision(uint64_t instance_id) const {
    for (uint32_t index = 0; index < goals_.size(); ++index) {
        const Goal &goal = goals_[index];
        if (goal.owner_instance_id == instance_id && is_live(goal.lifecycle) &&
            at_largest_revision(goal)) {
            return true;
        }
    }
    return false;
}

void GoalTable::cancel_owned(uint64_t instance_id, foundation::TransitionStamp stamp) {
    for (uint32_t index = 0; index < goals_.size(); ++index) {
        Goal &goal = goals_[index];
        if (goal.owner_instance_id == instance_id && is_live(goal.lifecycle)) {
            set_lifecycle(goal, AYNI_GOAL_CANCELLED, stamp);
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
    for (uint32_t index = 0; index < goals_.size(); ++index) {
        if (goals_[index].parent_index == parent_index && is_live(goals_[index].lifecycle)) {
            ++count;
        }
    }
    return count;
}

// Walks up at most depth_max parents.
bool GoalTable::in_subtree(uint32_t index, uint32_t root) const {
    const uint32_t root_depth = goals_[root].depth;
    uint32_t current = index;
    while (goals_[current].depth > root_depth) {
        current = goals_[current].parent_index;
    }
    return current == root;
}

bool GoalTable::at_largest_revision(const Goal &goal) {
    return goal.revision == max_revision;
}

bool GoalTable::descends_from(uint32_t index, uint32_t ancestor) const {
    return index != ancestor && in_subtree(index, ancestor);
}

// A terminal goal never changes again, so the one with the smaller
// commit_seq ended first. A descendant ends with its ancestor or before it,
// so of the goals that ended in one transition the deepest has no child
// left in the table.
bool GoalTable::gives_way_before(const Goal &goal, const Goal &other) {
    if (goal.commit_seq != other.commit_seq) {
        return goal.commit_seq < other.commit_seq;
    }
    if (goal.depth != other.depth) {
        return goal.depth > other.depth;
    }
    return goal.goal_id < other.goal_id;
}

bool GoalTable::allows(const Goal &goal, GoalChange change) {
    switch (change) {
    case GoalChange::pause:
        return goal.lifecycle == AYNI_GOAL_ACTIVE;
    case GoalChange::resume:
        return goal.lifecycle == AYNI_GOAL_PAUSED;
    case GoalChange::cancel:
    case GoalChange::expire:
        return is_live(goal.lifecycle);
    case GoalChange::satisfy:
    case GoalChange::fail:
        return goal.lifecycle == AYNI_GOAL_ACTIVE;
    }
    return false;
}

bool GoalTable::ends(GoalChange change) {
    return change != GoalChange::pause && change != GoalChange::resume;
}

void GoalTable::set_lifecycle(Goal &goal, ayni_goal_lifecycle_t lifecycle,
                              foundation::TransitionStamp stamp) {
    goal.lifecycle = lifecycle;
    ++goal.revision;
    goal.commit_seq = stamp.commit_seq;
    if (lifecycle == AYNI_GOAL_CANCELLED) {
        goal.cancel_revision = goal.revision;
    }
    if (!is_live(lifecycle)) {
        goal.cursor = {goal.cursor.taken, no_time};
        goal.expires_us = no_time;
    }
}

bool GoalTable::takes(const Goal &goal, const catalog::CatalogId &source) {
    return goal.trigger.kind == AYNI_TRIGGER_LOCAL_INPUT && is_live(goal.lifecycle) &&
           goal.trigger_source.equals({source.data(), source.size()}) && can_record(goal.cursor);
}

} // namespace ayni::work
