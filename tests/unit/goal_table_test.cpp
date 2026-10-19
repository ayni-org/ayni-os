// Goals owned by work/goal: the fixed owner (C-LIFE-020), the tree limits
// (C-LIMIT-007), revisions (C-COMMIT-012), pause and the cancel cascade
// (C-LIFE-021, C-LIFE-022), and the trigger cursor (ADR-0018).

#include "work/goal/goal_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using ayni::work::Goal;
using ayni::work::GoalChange;
using ayni::work::GoalTable;
using ayni::work::NewGoal;
using ayni::work::no_time;
using ayni::work::TakenOccurrences;
using ayni::work::Trigger;
using Admission = GoalTable::Admission;
using ChangeCheck = GoalTable::ChangeCheck;

constexpr uint64_t owner = 50;
constexpr Trigger no_trigger{AYNI_TRIGGER_NONE, 0, 0};
constexpr uint64_t other_owner = 60;

class Goals {
  public:
    explicit Goals(GoalTable::Limits limits)
        : storage_(GoalTable::storage_size(limits.capacity, limits.parameter_max_bytes) /
                       sizeof(uint64_t) +
                   1U),
          table_(storage_.data(), limits) {}

    GoalTable &table() { return table_; }
    GoalTable *operator->() { return &table_; }

    // Admits and inserts; returns the new goal's id, or 0 if refused.
    uint64_t create(uint64_t parent_goal_id = 0, uint64_t owner_instance_id = owner,
                    ayni_bytes_v1 parameters = {nullptr, 0}, Trigger trigger = no_trigger,
                    uint64_t created_us = 0, uint64_t expiry_us = 0) {
        const NewGoal goal =
            new_goal(parent_goal_id, owner_instance_id, parameters, trigger, created_us, expiry_us);
        if (table_.admit(goal) != Admission::admitted) {
            return 0;
        }
        const uint64_t goal_id = ++last_id_;
        uint32_t index = 0;
        EXPECT_TRUE(table_.insert(goal, goal_id, next_stamp(), index));
        return goal_id;
    }

    static NewGoal new_goal(uint64_t parent_goal_id, uint64_t owner_instance_id = owner,
                            ayni_bytes_v1 parameters = {nullptr, 0}, Trigger trigger = no_trigger,
                            uint64_t created_us = 0, uint64_t expiry_us = 0) {
        return {owner_instance_id,
                parent_goal_id,
                0,
                {AYNI_ORIGIN_USER, AYNI_CHANNEL_MANAGEMENT, 7, 1},
                parameters,
                trigger,
                created_us,
                expiry_us,
                ayni::work::no_time};
    }

    const Goal &goal(uint64_t goal_id) {
        uint32_t index = 0;
        EXPECT_TRUE(table_.find(goal_id, index));
        return table_.at(index);
    }

    uint32_t index_of(uint64_t goal_id) {
        uint32_t index = 0;
        EXPECT_TRUE(table_.find(goal_id, index));
        return index;
    }

    // Checks and applies; returns what check() said.
    ChangeCheck change(uint64_t goal_id, GoalChange change) {
        const uint32_t index = index_of(goal_id);
        const ChangeCheck check = table_.check(index, change, table_.at(index).revision);
        if (check == ChangeCheck::allowed) {
            table_.apply(index, change, next_stamp());
        }
        return check;
    }

    // White-box: the goals lie at the start of the storage (goal_table.h).
    Goal &raw(uint64_t goal_id) {
        return reinterpret_cast<Goal *>(storage_.data())[index_of(goal_id)];
    }

    ayni::foundation::TransitionStamp last_stamp() const { return {last_seq_ + 10U, last_seq_}; }

    ayni::foundation::TransitionStamp next_stamp() {
        ++last_seq_;
        return last_stamp();
    }

  private:
    std::vector<uint64_t> storage_;
    GoalTable table_;
    uint64_t last_id_ = 0;
    uint64_t last_seq_ = 0;
};

constexpr GoalTable::Limits small{8, 4, 2, 2};

TEST(GoalTable, new_goal_is_active_at_revision_1_with_its_own_copy_of_the_parameters) {
    Goals goals(small);
    std::vector<uint8_t> parameters{1, 2, 3};
    const uint64_t id = goals.create(0, owner, {parameters.data(), 3});
    ASSERT_NE(id, 0U);
    parameters[0] = 9;

    const Goal &goal = goals.goal(id);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goal.revision, 1U);
    EXPECT_EQ(goal.cancel_revision, 0U);
    EXPECT_EQ(goal.depth, 0U);
    EXPECT_EQ(goal.parent_index, GoalTable::no_parent);
    EXPECT_EQ(goal.owner_instance_id, owner);
    EXPECT_EQ(goal.created_transition_id, 11U);
    EXPECT_EQ(goal.commit_seq, 1U);
    EXPECT_EQ(goal.origin.principal_id, 7U);
    EXPECT_EQ(goal.trigger.kind, AYNI_TRIGGER_NONE);
    EXPECT_EQ(goal.cursor.taken, 0U);
    EXPECT_EQ(goal.cursor.next_due_us, no_time);
    EXPECT_EQ(goal.last_task_id, 0U);
    const ayni_bytes_v1 stored = goals->parameters(goals.index_of(id));
    ASSERT_EQ(stored.size, 3U);
    EXPECT_EQ(std::vector<uint8_t>(stored.data, stored.data + 3), (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_TRUE(goals->parameters_fit(4));
    EXPECT_FALSE(goals->parameters_fit(5));
}

TEST(GoalTable, children_sit_one_level_below_their_parent) {
    Goals goals(small);
    const uint64_t root = goals.create();
    const uint64_t child = goals.create(root);
    const uint64_t grandchild = goals.create(child);
    ASSERT_NE(grandchild, 0U);
    EXPECT_EQ(goals.goal(child).depth, 1U);
    EXPECT_EQ(goals.goal(grandchild).depth, 2U);
    EXPECT_EQ(goals.goal(grandchild).parent_goal_id, child);
    EXPECT_EQ(goals.goal(grandchild).parent_index, goals.index_of(child));
}

// C-LIFE-020: a child cannot move work to another owner.
TEST(GoalTable, child_must_have_its_parents_owner) {
    Goals goals(small);
    const uint64_t root = goals.create();
    EXPECT_EQ(goals->admit(Goals::new_goal(root, other_owner)), Admission::parent_owner);
    EXPECT_EQ(goals->admit(Goals::new_goal(root, owner)), Admission::admitted);
}

TEST(GoalTable, parent_must_exist_and_be_active_or_paused) {
    Goals goals(small);
    EXPECT_EQ(goals->admit(Goals::new_goal(99)), Admission::parent_unknown);
    const uint64_t root = goals.create();
    ASSERT_EQ(goals.change(root, GoalChange::pause), ChangeCheck::allowed);
    EXPECT_EQ(goals->admit(Goals::new_goal(root)), Admission::admitted);
    ASSERT_EQ(goals.change(root, GoalChange::cancel), ChangeCheck::allowed);
    EXPECT_EQ(goals->admit(Goals::new_goal(root)), Admission::parent_state);
}

// C-LIMIT-007: depth and the number of live children are bounded.
TEST(GoalTable, tree_limits_refuse_deeper_or_more_children) {
    Goals goals(small);
    const uint64_t root = goals.create();
    const uint64_t child = goals.create(root);
    const uint64_t grandchild = goals.create(child);
    EXPECT_EQ(goals->admit(Goals::new_goal(grandchild)), Admission::too_deep);

    ASSERT_NE(goals.create(root), 0U);
    EXPECT_EQ(goals->admit(Goals::new_goal(root)), Admission::too_many_children);
    // A terminal child no longer counts.
    ASSERT_EQ(goals.change(child, GoalChange::cancel), ChangeCheck::allowed);
    EXPECT_EQ(goals->admit(Goals::new_goal(root)), Admission::admitted);
}

TEST(GoalTable, zero_limits_allow_top_level_goals_only) {
    Goals no_depth({4, 0, 0, 2});
    const uint64_t root = no_depth.create();
    ASSERT_NE(root, 0U);
    EXPECT_EQ(no_depth->admit(Goals::new_goal(root)), Admission::too_deep);

    Goals no_children({4, 0, 2, 0});
    const uint64_t lonely = no_children.create();
    EXPECT_EQ(no_children->admit(Goals::new_goal(lonely)), Admission::too_many_children);
    EXPECT_FALSE(no_children->parameters_fit(1));
    EXPECT_TRUE(no_children->parameters_fit(0));
}

// Checks run parent, owner, depth, children, capacity: each refusal names
// the first rule broken.
TEST(GoalTable, admission_reports_the_first_rule_broken) {
    Goals goals({3, 0, 1, 1});
    const uint64_t root = goals.create();
    const uint64_t child = goals.create(root);
    ASSERT_NE(goals.create(), 0U);
    // Full, too deep and of another owner at once: the owner comes first.
    EXPECT_EQ(goals->admit(Goals::new_goal(child, other_owner)), Admission::parent_owner);
    // Full and too deep: depth comes first.
    EXPECT_EQ(goals->admit(Goals::new_goal(child)), Admission::too_deep);
    // Full and too many children: children come first.
    EXPECT_EQ(goals->admit(Goals::new_goal(root)), Admission::too_many_children);
    EXPECT_EQ(goals->admit(Goals::new_goal(0)), Admission::full);
}

// Terminal goals stay and count until the log defines their retention.
TEST(GoalTable, capacity_counts_terminal_goals) {
    Goals goals({2, 0, 2, 2});
    const uint64_t first = goals.create();
    ASSERT_NE(goals.create(), 0U);
    ASSERT_EQ(goals.change(first, GoalChange::cancel), ChangeCheck::allowed);
    EXPECT_EQ(goals->admit(Goals::new_goal(0)), Admission::full);
    EXPECT_EQ(goals.goal(first).lifecycle, AYNI_GOAL_CANCELLED);
}

// insert() checks the room itself, so a caller that skipped admit() cannot
// write past the table.
TEST(GoalTable, insert_into_a_full_table_changes_nothing) {
    Goals goals({1, 4, 2, 2});
    ASSERT_NE(goals.create(), 0U);
    const uint8_t bytes[] = {1, 2, 3, 4};
    uint32_t index = 77;
    EXPECT_FALSE(
        goals->insert(Goals::new_goal(0, owner, {bytes, sizeof(bytes)}), 99, {1, 1}, index));
    EXPECT_EQ(index, 77U);
    EXPECT_EQ(goals->size(), 1U);
    uint32_t found = 0;
    EXPECT_FALSE(goals->find(99, found));
}

// C-LIFE-021: pause keeps the goal and does not reach its children.
TEST(GoalTable, pause_and_resume_change_only_the_goal) {
    Goals goals(small);
    const uint64_t root = goals.create();
    const uint64_t child = goals.create(root);
    ASSERT_EQ(goals.change(root, GoalChange::pause), ChangeCheck::allowed);
    EXPECT_EQ(goals.goal(root).lifecycle, AYNI_GOAL_PAUSED);
    EXPECT_EQ(goals.goal(root).revision, 2U);
    EXPECT_EQ(goals.goal(root).commit_seq, goals.last_stamp().commit_seq);
    EXPECT_EQ(goals.goal(child).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goals.goal(child).revision, 1U);

    ASSERT_EQ(goals.change(root, GoalChange::resume), ChangeCheck::allowed);
    EXPECT_EQ(goals.goal(root).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goals.goal(root).revision, 3U);
    EXPECT_EQ(goals.goal(child).revision, 1U);
}

TEST(GoalTable, lifecycle_decides_which_changes_are_allowed) {
    Goals goals(small);
    const uint64_t id = goals.create();
    EXPECT_EQ(goals.change(id, GoalChange::resume), ChangeCheck::invalid_state);
    ASSERT_EQ(goals.change(id, GoalChange::pause), ChangeCheck::allowed);
    EXPECT_EQ(goals.change(id, GoalChange::pause), ChangeCheck::invalid_state);
    ASSERT_EQ(goals.change(id, GoalChange::cancel), ChangeCheck::allowed);
    for (const GoalChange change : {GoalChange::pause, GoalChange::resume, GoalChange::cancel}) {
        EXPECT_EQ(goals.change(id, change), ChangeCheck::invalid_state);
    }
    EXPECT_EQ(goals.goal(id).revision, 3U);
}

// C-LIFE-022: cancelling reaches every live descendant in one transition and
// nothing else; a terminal descendant keeps its revision.
TEST(GoalTable, cancel_reaches_every_live_descendant_and_nothing_else) {
    Goals goals({8, 0, 2, 2});
    const uint64_t root = goals.create();
    const uint64_t child = goals.create(root);
    const uint64_t grandchild = goals.create(child);
    const uint64_t done_child = goals.create(root);
    const uint64_t other_root = goals.create();
    const uint64_t other_child = goals.create(other_root);
    ASSERT_EQ(goals.change(done_child, GoalChange::cancel), ChangeCheck::allowed);
    ASSERT_EQ(goals.change(child, GoalChange::pause), ChangeCheck::allowed);

    ASSERT_EQ(goals.change(root, GoalChange::cancel), ChangeCheck::allowed);
    const uint64_t seq = goals.last_stamp().commit_seq;
    for (const uint64_t id : {root, child, grandchild}) {
        EXPECT_EQ(goals.goal(id).lifecycle, AYNI_GOAL_CANCELLED) << id;
        EXPECT_EQ(goals.goal(id).commit_seq, seq) << id;
        EXPECT_EQ(goals.goal(id).cancel_revision, goals.goal(id).revision) << id;
    }
    EXPECT_EQ(goals.goal(root).revision, 2U);
    EXPECT_EQ(goals.goal(child).revision, 3U);
    EXPECT_EQ(goals.goal(grandchild).revision, 2U);
    EXPECT_EQ(goals.goal(done_child).revision, 2U);
    EXPECT_NE(goals.goal(done_child).commit_seq, seq);
    EXPECT_EQ(goals.goal(other_root).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goals.goal(other_child).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goals.goal(other_child).revision, 1U);
}

// C-COMMIT-012: a stale revision is a conflict, whatever the state.
TEST(GoalTable, stale_revision_is_a_conflict_before_the_state_is_checked) {
    Goals goals(small);
    const uint64_t id = goals.create();
    const uint32_t index = goals.index_of(id);
    EXPECT_EQ(goals->check(index, GoalChange::pause, 0), ChangeCheck::revision_conflict);
    EXPECT_EQ(goals->check(index, GoalChange::pause, 2), ChangeCheck::revision_conflict);
    EXPECT_EQ(goals->check(index, GoalChange::resume, 2), ChangeCheck::revision_conflict);
    EXPECT_EQ(goals->check(index, GoalChange::resume, 1), ChangeCheck::invalid_state);
    EXPECT_EQ(goals->check(index, GoalChange::pause, 1), ChangeCheck::allowed);
}

// A revision never wraps; the change that would raise one past the largest
// value is refused, also when it is a descendant's.
TEST(GoalTable, change_past_the_largest_revision_is_refused) {
    constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();
    Goals goals(small);
    const uint64_t root = goals.create();
    const uint64_t child = goals.create(root);
    goals.raw(child).revision = max_revision;
    EXPECT_EQ(goals.change(child, GoalChange::pause), ChangeCheck::revision_exhausted);
    EXPECT_EQ(goals.change(root, GoalChange::cancel), ChangeCheck::revision_exhausted);
    EXPECT_EQ(goals.goal(root).lifecycle, AYNI_GOAL_ACTIVE);
    // Pausing the root raises only the root's revision.
    EXPECT_EQ(goals.change(root, GoalChange::pause), ChangeCheck::allowed);
    goals.raw(root).revision = max_revision;
    EXPECT_EQ(goals.change(root, GoalChange::resume), ChangeCheck::revision_exhausted);
}

// ADR-0018, ADR-0019: ONCE has one occurrence, due at creation, and NONE has
// none; taking it gives the goal a new commit_seq and keeps its revision.
TEST(GoalTable, once_has_one_occurrence_due_at_creation_and_none_has_none) {
    Goals goals(small);
    const uint64_t none = goals.create(0, owner, {nullptr, 0}, no_trigger, 500);
    const uint64_t once = goals.create(0, owner, {nullptr, 0}, {AYNI_TRIGGER_ONCE, 0, 0}, 500);
    EXPECT_FALSE(goals.goal(none).cursor.due_by(no_time - 1U));
    EXPECT_FALSE(goals.goal(once).cursor.due_by(499));
    ASSERT_TRUE(goals.goal(once).cursor.due_by(500));

    const TakenOccurrences taken =
        goals->take_due(goals.index_of(once), 900, 77, goals.next_stamp());
    EXPECT_EQ(taken.latest, 1U);
    EXPECT_EQ(taken.count, 1U);
    EXPECT_EQ(taken.due_us, 500U);
    const Goal &goal = goals.goal(once);
    EXPECT_EQ(goal.trigger.kind, AYNI_TRIGGER_ONCE);
    EXPECT_EQ(goal.cursor.taken, 1U);
    EXPECT_FALSE(goal.cursor.has_next());
    EXPECT_EQ(goal.last_task_id, 77U);
    EXPECT_EQ(goal.revision, 1U);
    EXPECT_EQ(goal.commit_seq, goals.last_stamp().commit_seq);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_ACTIVE);
}

// A terminal goal has no occurrence to come; a paused one keeps its schedule.
TEST(GoalTable, only_a_terminal_goal_drops_its_next_occurrence) {
    Goals goals(small);
    const Trigger every_second{AYNI_TRIGGER_INTERVAL, 0, AYNI_TRIGGER_PERIOD_MIN_US};
    const uint64_t root = goals.create(0, owner, {nullptr, 0}, every_second, 10);
    const uint64_t child = goals.create(root, owner, {nullptr, 0}, every_second, 20);
    EXPECT_EQ(goals.change(root, GoalChange::pause), ChangeCheck::allowed);
    EXPECT_EQ(goals.goal(root).cursor.next_due_us, 10U);
    EXPECT_EQ(goals.change(root, GoalChange::cancel), ChangeCheck::allowed);
    EXPECT_FALSE(goals.goal(root).cursor.has_next());
    EXPECT_FALSE(goals.goal(child).cursor.has_next());
}

TEST(GoalTable, subtree_holds_the_goal_and_its_descendants_only) {
    Goals goals(small);
    const uint64_t root = goals.create();
    const uint64_t child = goals.create(root);
    const uint64_t grandchild = goals.create(child);
    const uint64_t other = goals.create();
    const uint32_t root_index = goals.index_of(root);
    for (const uint64_t id : {root, child, grandchild}) {
        EXPECT_TRUE(goals->in_subtree(goals.index_of(id), root_index)) << id;
    }
    EXPECT_FALSE(goals->in_subtree(goals.index_of(other), root_index));
    EXPECT_FALSE(goals->in_subtree(root_index, goals.index_of(child)));
}

// Stopping an instance cancels its goals that are not terminal (ADR-0018);
// another owner's goals and terminal goals keep their revision.
TEST(GoalTable, cancel_owned_reaches_the_owners_live_goals_only) {
    Goals goals(small);
    const uint64_t root = goals.create();
    const uint64_t child = goals.create(root);
    const uint64_t done = goals.create();
    const uint64_t foreign = goals.create(0, other_owner);
    ASSERT_EQ(goals.change(done, GoalChange::cancel), ChangeCheck::allowed);
    ASSERT_EQ(goals.change(child, GoalChange::pause), ChangeCheck::allowed);
    EXPECT_FALSE(goals->owned_at_largest_revision(owner));

    goals->cancel_owned(owner, goals.next_stamp());
    const uint64_t seq = goals.last_stamp().commit_seq;
    for (const uint64_t id : {root, child}) {
        EXPECT_EQ(goals.goal(id).lifecycle, AYNI_GOAL_CANCELLED) << id;
        EXPECT_EQ(goals.goal(id).commit_seq, seq) << id;
        EXPECT_EQ(goals.goal(id).cancel_revision, goals.goal(id).revision) << id;
    }
    EXPECT_EQ(goals.goal(child).revision, 3U);
    EXPECT_EQ(goals.goal(done).revision, 2U);
    EXPECT_EQ(goals.goal(foreign).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goals.goal(foreign).revision, 1U);
}

TEST(GoalTable, owned_at_largest_revision_ignores_terminal_and_foreign_goals) {
    constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();
    Goals goals(small);
    const uint64_t live = goals.create();
    const uint64_t done = goals.create();
    const uint64_t foreign = goals.create(0, other_owner);
    ASSERT_EQ(goals.change(done, GoalChange::cancel), ChangeCheck::allowed);
    goals.raw(done).revision = max_revision;
    goals.raw(foreign).revision = max_revision;
    EXPECT_FALSE(goals->owned_at_largest_revision(owner));
    goals.raw(live).revision = max_revision;
    EXPECT_TRUE(goals->owned_at_largest_revision(owner));
}

} // namespace
