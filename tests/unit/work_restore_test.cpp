// Recovery of the work model from the log (ADR-0026): the restore functions
// of the agent, goal and task tables accept what a committed transition
// left and refuse, changing nothing, what no transition writes (corrupt) or
// what this boot's limits cannot hold (no_room); after the last record they
// fault, hold and re-time what came back (ADR-0026 decisions 6 and 7). The
// replay refuses a batch that created a goal or task without its state, and
// each transition journals its records in replay order within
// transition_batch_max().

#include "durability/batch/batch_codec.h"
#include "durability/journal/journal_ring.h"
#include "foundation/kernel_id.h"
#include "kernel/command/command_records.h"
#include "kernel/intent/work_model.h"
#include "kernel/work_journal.h"
#include "kernel/work_replay.h"
#include "work/agent/agent_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_records.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using ayni::catalog::CatalogId;
using ayni::durability::BatchCheck;
using ayni::durability::BatchView;
using ayni::durability::BatchWriter;
using ayni::durability::record_required;
using ayni::durability::RecordHeader;
using ayni::durability::RecordReader;
using ayni::durability::RecordUse;
using ayni::foundation::IdAllocator;
using ayni::foundation::TransitionStamp;
using ayni::kernel::CommandMapping;
using ayni::kernel::KernelCapacities;
using ayni::kernel::KernelStorage;
using ayni::kernel::WorkModel;
using ayni::kernel::WorkReplay;
using ayni::work::AgentInstance;
using ayni::work::AgentTable;
using ayni::work::Goal;
using ayni::work::GoalChange;
using ayni::work::GoalState;
using ayni::work::GoalTable;
using ayni::work::NewGoal;
using ayni::work::NewTask;
using ayni::work::no_time;
using ayni::work::Task;
using ayni::work::TaskChange;
using ayni::work::TaskState;
using ayni::work::TaskTable;
using ayni::work::Trigger;

constexpr uint64_t owner = 0x1'0000'0002ULL;
constexpr uint64_t owner_agent = 0x41;
constexpr uint64_t other_owner = 0x1'0000'0003ULL;
constexpr uint64_t principal = 7;
constexpr Trigger no_trigger{AYNI_TRIGGER_NONE, 0, 0};
constexpr const char *long_id = "abcdefghijklmnopqrstuvwxyz.01234"; // 32 bytes

CatalogId catalog_id(const char *text) {
    CatalogId id;
    EXPECT_TRUE(id.assign({text, static_cast<uint32_t>(std::strlen(text))}));
    return id;
}

ayni_string_v1 key(const char *text) {
    return {text, static_cast<uint32_t>(std::strlen(text))};
}

// The stamp of the transition, and so of the batch, commit_seq.
TransitionStamp stamp(uint64_t commit_seq) {
    return {commit_seq + 100U, commit_seq};
}

// ---- Agents ---------------------------------------------------------------

using AgentRestore = AgentTable::Restore;

class Agents {
  public:
    explicit Agents(uint32_t capacity)
        : storage_(AgentTable::storage_size(capacity) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), capacity) {}

    AgentTable *operator->() { return &table_; }

    AgentRestore restore(uint64_t agent_id, uint64_t instance_id, uint32_t revision,
                         ayni_agent_lifecycle_t lifecycle, uint64_t seq = 1) {
        return table_.restore(agent_id, instance_id, revision, lifecycle, stamp(seq));
    }

    const AgentInstance &agent(uint64_t agent_id) {
        const AgentInstance *found = table_.find_agent(agent_id);
        EXPECT_NE(found, nullptr) << agent_id;
        return found != nullptr ? *found : table_.at(0);
    }

  private:
    std::vector<uint64_t> storage_;
    AgentTable table_;
};

// ADR-0026: the log keeps an agent's instance across restarts; a registered
// agent gets the instance its last record left, with that record's stamp.
TEST(AgentRestore, registered_agent_gets_its_logged_instance) {
    Agents agents(2);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    ASSERT_EQ(agents->add(200), AgentTable::AddResult::added);
    ASSERT_EQ(agents.restore(100, 0x1'0000'0001ULL, 1, AYNI_AGENT_RUNNING, 2),
              AgentRestore::restored);
    ASSERT_EQ(agents.restore(100, 0x1'0000'0001ULL, 3, AYNI_AGENT_PAUSED, 5),
              AgentRestore::restored);

    const AgentInstance &agent = agents.agent(100);
    EXPECT_EQ(agent.instance_id, 0x1'0000'0001ULL);
    EXPECT_EQ(agent.revision, 3U);
    EXPECT_EQ(agent.lifecycle, AYNI_AGENT_PAUSED);
    EXPECT_EQ(agent.stamp.commit_seq, 5U);
    EXPECT_EQ(agent.stamp.transition_id, 105U);
    EXPECT_TRUE(agent.registered);
    EXPECT_EQ(agent.fault_reason, AYNI_REASON_NONE);
    EXPECT_EQ(agents->size(), 2U);
    // Agent 200 has no record yet: it starts with the first transition.
    EXPECT_TRUE(agents->has_registered());
    ASSERT_EQ(agents.restore(200, 0x1'0000'0007ULL, 2, AYNI_AGENT_STOPPED), AgentRestore::restored);
    EXPECT_FALSE(agents->has_registered());
}

// An INSTANCE record names a started instance: ids, a revision of at least
// 1 and a lifecycle a transition leaves (ADR-0018).
TEST(AgentRestore, record_no_transition_writes_is_corrupt) {
    Agents agents(2);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    EXPECT_EQ(agents.restore(0, 0x1'0000'0001ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::corrupt);
    EXPECT_EQ(agents.restore(100, 0, 1, AYNI_AGENT_RUNNING), AgentRestore::corrupt);
    EXPECT_EQ(agents.restore(100, 0x1'0000'0001ULL, 0, AYNI_AGENT_RUNNING), AgentRestore::corrupt);
    for (const ayni_agent_lifecycle_t lifecycle :
         {ayni_agent_lifecycle_t{0}, AYNI_AGENT_REGISTERED, AYNI_AGENT_STARTING,
          AYNI_AGENT_STOPPING, AYNI_AGENT_FAULTED, ayni_agent_lifecycle_t{8}}) {
        EXPECT_EQ(agents.restore(100, 0x1'0000'0001ULL, 1, lifecycle), AgentRestore::corrupt)
            << lifecycle;
    }
    EXPECT_EQ(agents->size(), 1U);
    EXPECT_EQ(agents.agent(100).lifecycle, AYNI_AGENT_REGISTERED);
    EXPECT_EQ(agents.agent(100).instance_id, 0U);

    // A stopped instance never changes again, and no change lowers a
    // revision.
    ASSERT_EQ(agents.restore(100, 0x1'0000'0001ULL, 3, AYNI_AGENT_RUNNING, 2),
              AgentRestore::restored);
    EXPECT_EQ(agents.restore(100, 0x1'0000'0001ULL, 2, AYNI_AGENT_PAUSED, 3),
              AgentRestore::corrupt);
    ASSERT_EQ(agents.restore(100, 0x1'0000'0001ULL, 4, AYNI_AGENT_STOPPED, 3),
              AgentRestore::restored);
    EXPECT_EQ(agents.restore(100, 0x1'0000'0001ULL, 5, AYNI_AGENT_RUNNING, 4),
              AgentRestore::corrupt);
    EXPECT_EQ(agents.agent(100).lifecycle, AYNI_AGENT_STOPPED);
    EXPECT_EQ(agents.agent(100).revision, 4U);
}

// C-ID-010: an agent has one instance for its life, and an instance id one
// agent.
TEST(AgentRestore, instance_stays_with_one_agent) {
    Agents agents(3);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    ASSERT_EQ(agents->add(200), AgentTable::AddResult::added);
    ASSERT_EQ(agents.restore(100, 0x1'0000'0001ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::restored);
    EXPECT_EQ(agents.restore(100, 0x1'0000'0002ULL, 2, AYNI_AGENT_RUNNING), AgentRestore::corrupt);
    EXPECT_EQ(agents.restore(200, 0x1'0000'0001ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::corrupt);
    EXPECT_EQ(agents.restore(300, 0x1'0000'0001ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::corrupt);
    EXPECT_EQ(agents->size(), 2U);
    EXPECT_EQ(agents.agent(100).instance_id, 0x1'0000'0001ULL);
    EXPECT_EQ(agents.agent(100).revision, 1U);
    EXPECT_EQ(agents.agent(200).instance_id, 0U);
}

// ADR-0026 decision 7: an instance whose agent this boot did not register
// keeps its record in a row of its own, while the table has one.
TEST(AgentRestore, unregistered_agent_gets_a_row_while_there_is_room) {
    Agents agents(2);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    ASSERT_EQ(agents.restore(300, 0x1'0000'0005ULL, 2, AYNI_AGENT_RUNNING), AgentRestore::restored);
    const AgentInstance &orphan = agents.agent(300);
    EXPECT_FALSE(orphan.registered);
    EXPECT_EQ(orphan.instance_id, 0x1'0000'0005ULL);
    EXPECT_EQ(orphan.lifecycle, AYNI_AGENT_RUNNING);
    // A later record of the same instance finds its row.
    ASSERT_EQ(agents.restore(300, 0x1'0000'0005ULL, 3, AYNI_AGENT_PAUSED, 4),
              AgentRestore::restored);
    EXPECT_EQ(agents->size(), 2U);
    EXPECT_EQ(agents.agent(300).lifecycle, AYNI_AGENT_PAUSED);
    EXPECT_EQ(agents.restore(400, 0x1'0000'0006ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::no_room);
    EXPECT_EQ(agents->find_agent(400), nullptr);
}

// ADR-0026 decision 7: after the last record, a live instance whose agent
// is not registered is FAULTED for AGENT_UNREGISTERED in memory, with its
// revision and stamp as logged; a STOPPED one stays STOPPED, and registered
// agents keep theirs.
TEST(AgentRestore, fault_unregistered_faults_live_orphans_only) {
    Agents agents(5);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    ASSERT_EQ(agents.restore(100, 0x1'0000'0001ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::restored);
    ASSERT_EQ(agents.restore(300, 0x1'0000'0003ULL, 4, AYNI_AGENT_RUNNING, 6),
              AgentRestore::restored);
    ASSERT_EQ(agents.restore(400, 0x1'0000'0004ULL, 2, AYNI_AGENT_PAUSED), AgentRestore::restored);
    ASSERT_EQ(agents.restore(500, 0x1'0000'0005ULL, 3, AYNI_AGENT_STOPPED), AgentRestore::restored);
    agents->fault_unregistered();

    for (const uint64_t faulted : {300U, 400U}) {
        EXPECT_EQ(agents.agent(faulted).lifecycle, AYNI_AGENT_FAULTED) << faulted;
        EXPECT_EQ(agents.agent(faulted).fault_reason, AYNI_REASON_AGENT_UNREGISTERED) << faulted;
    }
    EXPECT_EQ(agents.agent(300).revision, 4U);
    EXPECT_EQ(agents.agent(300).stamp.commit_seq, 6U);
    EXPECT_EQ(agents.agent(500).lifecycle, AYNI_AGENT_STOPPED);
    EXPECT_EQ(agents.agent(500).fault_reason, AYNI_REASON_NONE);
    EXPECT_EQ(agents.agent(100).lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(agents.agent(100).fault_reason, AYNI_REASON_NONE);
}

// A recovery that failed leaves the agents as registration left them: no
// instances, and no rows for agents this boot did not register.
TEST(AgentRestore, drop_restored_returns_to_the_registered_agents) {
    Agents agents(4);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    ASSERT_EQ(agents->add(200), AgentTable::AddResult::added);
    ASSERT_EQ(agents.restore(100, 0x1'0000'0001ULL, 2, AYNI_AGENT_PAUSED), AgentRestore::restored);
    ASSERT_EQ(agents.restore(300, 0x1'0000'0003ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::restored);
    ASSERT_EQ(agents.restore(400, 0x1'0000'0004ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::restored);
    agents->fault_unregistered();
    agents->drop_restored();

    EXPECT_EQ(agents->size(), 2U);
    EXPECT_EQ(agents->find_agent(300), nullptr);
    EXPECT_EQ(agents->find_agent(400), nullptr);
    for (const uint64_t agent_id : {100U, 200U}) {
        const AgentInstance &agent = agents.agent(agent_id);
        EXPECT_EQ(agent.lifecycle, AYNI_AGENT_REGISTERED) << agent_id;
        EXPECT_EQ(agent.instance_id, 0U);
        EXPECT_EQ(agent.revision, 0U);
        EXPECT_EQ(agent.stamp.commit_seq, 0U);
        EXPECT_TRUE(agent.registered);
    }
    EXPECT_EQ(agents->find_instance(0x1'0000'0001ULL), nullptr);
    // The instances start anew, and another recovery finds room again.
    IdAllocator ids;
    ids.start(2);
    agents->start_all(ids, stamp(1));
    EXPECT_EQ(agents.agent(100).lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(agents.restore(300, 0x1'0000'0003ULL, 1, AYNI_AGENT_RUNNING), AgentRestore::restored);
}

// ---- Goals ----------------------------------------------------------------

using GoalRestore = GoalTable::Restore;

NewGoal new_goal(uint64_t parent_goal_id = 0, Trigger trigger = no_trigger, uint64_t expiry_us = 0,
                 ayni_bytes_v1 parameters = {nullptr, 0}) {
    return {owner,
            parent_goal_id,
            0,
            {AYNI_ORIGIN_USER, AYNI_CHANNEL_MANAGEMENT, principal, 1},
            parameters,
            trigger,
            {},
            0,
            expiry_us,
            no_time,
            catalog_id("reminder"),
            1};
}

NewGoal input_goal(const char *source) {
    NewGoal goal = new_goal(0, {AYNI_TRIGGER_LOCAL_INPUT, 0, 0});
    goal.trigger_source = catalog_id(source);
    return goal;
}

GoalState goal_state(ayni_goal_lifecycle_t lifecycle, uint32_t revision, uint32_t taken = 0,
                     bool next_scheduled = false) {
    GoalState state{};
    state.lifecycle = lifecycle;
    state.revision = revision;
    state.taken = taken;
    state.next_scheduled = next_scheduled;
    state.skip_reason = AYNI_REASON_NONE;
    return state;
}

class Goals {
  public:
    explicit Goals(GoalTable::Limits limits)
        : storage_(GoalTable::storage_size(limits.capacity, limits.parameter_max_bytes) /
                       sizeof(uint64_t) +
                   1U),
          table_(storage_.data(), limits) {}

    GoalTable *operator->() { return &table_; }

    // Replays a GOAL record of the transition seq; last_index() is where the
    // goal went.
    GoalRestore restore(uint64_t goal_id, const NewGoal &goal = new_goal(),
                        uint64_t replaced_goal_id = 0, uint64_t seq = 1) {
        last_index_ = GoalTable::no_index;
        return table_.restore(goal, goal_id, replaced_goal_id, stamp(seq), last_index_);
    }
    uint32_t last_index() const { return last_index_; }

    // A goal and the state its creation left, in the transition seq.
    void created(uint64_t goal_id, const NewGoal &goal = new_goal(), uint64_t seq = 1,
                 GoalState state = goal_state(AYNI_GOAL_ACTIVE, 1)) {
        ASSERT_EQ(restore(goal_id, goal, 0, seq), GoalRestore::restored) << goal_id;
        ASSERT_EQ(table_.restore_state(last_index_, state, stamp(seq)), GoalRestore::restored);
    }

    GoalRestore state(uint64_t goal_id, const GoalState &state, uint64_t seq = 2) {
        return table_.restore_state(index_of(goal_id), state, stamp(seq));
    }

    // Ends the goal by the GOAL_STATE of a cancel.
    void cancel(uint64_t goal_id, uint64_t seq) {
        GoalState cancelled = goal_state(AYNI_GOAL_CANCELLED, goal(goal_id).revision + 1U);
        cancelled.cancel_revision = cancelled.revision;
        cancelled.taken = goal(goal_id).cursor.taken;
        ASSERT_EQ(state(goal_id, cancelled, seq), GoalRestore::restored);
    }

    bool exists(uint64_t goal_id) const {
        uint32_t index = 0;
        return table_.find(goal_id, index);
    }

    uint32_t index_of(uint64_t goal_id) const {
        uint32_t index = 0;
        EXPECT_TRUE(table_.find(goal_id, index)) << goal_id;
        return index;
    }

    const Goal &goal(uint64_t goal_id) const { return table_.at(index_of(goal_id)); }

    std::vector<uint8_t> parameters(uint64_t goal_id) const {
        const ayni_bytes_v1 stored = table_.parameters(index_of(goal_id));
        return {stored.data, stored.data + stored.size};
    }

  private:
    std::vector<uint64_t> storage_;
    GoalTable table_;
    uint32_t last_index_ = GoalTable::no_index;
};

// ADR-0026: a GOAL record replays the goal as insert() created it in the
// record's stamp: ACTIVE at revision 1, with its own copy of the
// parameters, in the tree under its parent.
TEST(GoalRestore, restore_creates_the_goal_as_insert_did_in_its_stamp) {
    Goals goals({4, 4, 2, 2});
    const uint8_t parameters[] = {1, 2, 3};
    ASSERT_EQ(
        goals.restore(5, new_goal(0, {AYNI_TRIGGER_DELAY, 100, 0}, 50, {parameters, 3}), 0, 7),
        GoalRestore::restored);
    const Goal &goal = goals.goal(5);
    EXPECT_EQ(goals.last_index(), goals.index_of(5));
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goal.revision, 1U);
    EXPECT_EQ(goal.cancel_revision, 0U);
    EXPECT_EQ(goal.commit_seq, 7U);
    EXPECT_EQ(goal.created_transition_id, 107U);
    EXPECT_EQ(goal.replaced_goal_id, 0U);
    EXPECT_EQ(goal.owner_instance_id, owner);
    EXPECT_EQ(goal.origin.principal_id, principal);
    EXPECT_EQ(goal.depth, 0U);
    EXPECT_EQ(goal.parent_index, GoalTable::no_parent);
    EXPECT_TRUE(goal.goal_type_id.equals(key("reminder")));
    EXPECT_EQ(goal.goal_type_version, 1U);
    EXPECT_EQ(goal.trigger.kind, AYNI_TRIGGER_DELAY);
    EXPECT_EQ(goal.trigger.delay_us, 100U);
    EXPECT_EQ(goal.expiry_after_us, 50U);
    EXPECT_EQ(goals.parameters(5), (std::vector<uint8_t>{1, 2, 3}));

    ASSERT_EQ(goals.restore(6, new_goal(5), 0, 8), GoalRestore::restored);
    EXPECT_EQ(goals.goal(6).depth, 1U);
    EXPECT_EQ(goals.goal(6).parent_index, goals.index_of(5));
    EXPECT_EQ(goals.goal(6).parent_goal_id, 5U);
}

// ADR-0025: a goal that took a terminal goal's place takes that place
// again, with its parameter block, also on a boot with more room.
TEST(GoalRestore, restore_takes_the_place_of_the_goal_it_replaced) {
    for (const uint32_t capacity : {2U, 3U}) {
        Goals goals({capacity, 2, 2, 2});
        const uint8_t first[] = {1};
        const uint8_t second[] = {2, 2};
        const uint8_t third[] = {3, 3};
        goals.created(1, new_goal(0, no_trigger, 0, {first, 1}), 1);
        goals.created(2, new_goal(0, no_trigger, 0, {second, 2}), 2);
        goals.cancel(1, 3);
        const uint32_t place = goals.index_of(1);

        ASSERT_EQ(goals.restore(3, new_goal(0, no_trigger, 0, {third, 2}), 1, 4),
                  GoalRestore::restored)
            << capacity;
        EXPECT_EQ(goals.last_index(), place);
        EXPECT_EQ(goals.index_of(3), place);
        EXPECT_FALSE(goals.exists(1));
        EXPECT_EQ(goals->size(), 2U);
        EXPECT_EQ(goals.goal(3).replaced_goal_id, 1U);
        EXPECT_EQ(goals.parameters(3), (std::vector<uint8_t>{3, 3}));
        EXPECT_EQ(goals.parameters(2), (std::vector<uint8_t>{2, 2}));
    }
}

// A GOAL record no transition writes is corrupt and changes nothing: no id,
// no version, a trigger whose times do not fit its kind or whose kind the
// catalog does not define, an id already taken, a parent that is unknown,
// terminal or of another owner, a replaced goal that is unknown or live.
TEST(GoalRestore, restore_refuses_a_goal_no_transition_creates) {
    Goals goals({6, 4, 2, 2});
    goals.created(1);
    goals.created(2);
    goals.cancel(2, 2);
    NewGoal foreign = new_goal();
    foreign.owner_instance_id = other_owner;
    goals.created(3, foreign, 3);

    NewGoal no_version = new_goal();
    no_version.goal_type_version = 0;
    const struct {
        const char *what;
        uint64_t goal_id;
        NewGoal goal;
        uint64_t replaced_goal_id;
    } cases[] = {
        {"no id", 0, new_goal(), 0},
        {"no version", 9, no_version, 0},
        {"delay with a period", 9, new_goal(0, {AYNI_TRIGGER_DELAY, 5, 1'000'000}), 0},
        {"interval below the period minimum", 9, new_goal(0, {AYNI_TRIGGER_INTERVAL, 0, 1}), 0},
        {"once with a delay", 9, new_goal(0, {AYNI_TRIGGER_ONCE, 5, 0}), 0},
        {"undefined trigger kind", 9, new_goal(0, {ayni_trigger_kind_t{9}, 0, 0}), 0},
        {"id taken", 1, new_goal(), 0},
        {"unknown parent", 9, new_goal(99), 0},
        {"terminal parent", 9, new_goal(2), 0},
        {"parent of another owner", 9, new_goal(3), 0},
        {"unknown replaced goal", 9, new_goal(), 99},
        {"live replaced goal", 9, new_goal(), 1},
    };
    for (const auto &item : cases) {
        EXPECT_EQ(goals.restore(item.goal_id, item.goal, item.replaced_goal_id),
                  GoalRestore::corrupt)
            << item.what;
    }
    EXPECT_EQ(goals->size(), 3U);
    EXPECT_FALSE(goals.exists(9));
    EXPECT_EQ(goals.goal(1).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goals.goal(2).lifecycle, AYNI_GOAL_CANCELLED);
}

// C-LIMIT-007: the log may come from a boot with larger limits; a goal this
// boot cannot hold is no_room, checked as admit() checks a new goal, and
// changes nothing.
TEST(GoalRestore, restore_reports_what_this_boots_limits_cannot_hold) {
    Goals goals({3, 2, 1, 1});
    goals.created(1);
    const uint8_t parameters[] = {1, 2, 3};
    EXPECT_EQ(goals.restore(4, new_goal(0, no_trigger, 0, {parameters, 3})), GoalRestore::no_room);
    EXPECT_EQ(goals.restore(4, new_goal(0, no_trigger, 0, {parameters, 2})), GoalRestore::restored);
    goals.created(2, new_goal(1), 2);
    EXPECT_EQ(goals.restore(5, new_goal(2)), GoalRestore::no_room); // too deep
    EXPECT_EQ(goals.restore(5, new_goal(1)), GoalRestore::no_room); // too many children
    EXPECT_EQ(goals->size(), 3U);
    // A terminal child no longer counts, but the table is full.
    goals.cancel(2, 3);
    EXPECT_EQ(goals.restore(5, new_goal(1)), GoalRestore::no_room);
    EXPECT_EQ(goals.restore(5, new_goal(1), 2), GoalRestore::restored);
    EXPECT_EQ(goals.goal(5).depth, 1U);
    EXPECT_EQ(goals->size(), 3U);
}

// ADR-0026: a GOAL_STATE record replays what a change left, in its stamp;
// taking an occurrence keeps the revision, so an equal one is accepted.
// Whether a next occurrence and an expiry are pending stands for times that
// reanchor() sets.
TEST(GoalRestore, restore_state_replays_what_a_change_left) {
    Goals goals({4, 0, 2, 2});
    goals.created(1, new_goal(0, {AYNI_TRIGGER_INTERVAL, 30, 1'000'000}, 500), 1,
                  goal_state(AYNI_GOAL_ACTIVE, 1, 0, true));
    GoalState paused{9, AYNI_GOAL_PAUSED, 3, 0, 2, AYNI_REASON_TASK_CAPACITY, 4, 0, true, true};
    ASSERT_EQ(goals.state(1, paused, 6), GoalRestore::restored);
    const Goal &goal = goals.goal(1);
    EXPECT_EQ(goal.last_task_id, 9U);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_PAUSED);
    EXPECT_EQ(goal.revision, 3U);
    EXPECT_EQ(goal.cancel_revision, 0U);
    EXPECT_EQ(goal.occurrences_skipped, 2U);
    EXPECT_EQ(goal.skip_reason, AYNI_REASON_TASK_CAPACITY);
    EXPECT_EQ(goal.commit_seq, 6U);
    EXPECT_EQ(goal.cursor.taken, 4U);
    EXPECT_EQ(goal.cursor.waiting, 0U);
    EXPECT_TRUE(goal.cursor.has_next());
    EXPECT_NE(goal.expires_us, no_time);

    paused.taken = 5;
    paused.next_scheduled = false;
    paused.expiry_pending = false;
    ASSERT_EQ(goals.state(1, paused, 7), GoalRestore::restored);
    EXPECT_EQ(goals.goal(1).revision, 3U);
    EXPECT_EQ(goals.goal(1).commit_seq, 7U);
    EXPECT_FALSE(goals.goal(1).cursor.has_next());
    EXPECT_EQ(goals.goal(1).expires_us, no_time);

    GoalState cancelled = goal_state(AYNI_GOAL_CANCELLED, 4, 5);
    cancelled.cancel_revision = 4;
    ASSERT_EQ(goals.state(1, cancelled, 8), GoalRestore::restored);
    EXPECT_EQ(goals.goal(1).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(goals.goal(1).cancel_revision, 4U);

    // ADR-0021: a local input trigger's recorded occurrences wait to be
    // taken.
    goals.created(2, input_goal("button.top"), 9);
    GoalState waiting = goal_state(AYNI_GOAL_ACTIVE, 1, 3, true);
    waiting.waiting = 2;
    ASSERT_EQ(goals.state(2, waiting, 10), GoalRestore::restored);
    EXPECT_EQ(goals.goal(2).cursor.waiting, 2U);
    EXPECT_EQ(goals.goal(2).cursor.taken, 3U);
    EXPECT_TRUE(goals.goal(2).cursor.has_next());
    EXPECT_NE(goals.goal(2).cursor.latest_due_us, no_time);
}

// A GOAL_STATE record no transition writes is corrupt and changes nothing.
TEST(GoalRestore, restore_state_refuses_a_state_no_transition_writes) {
    Goals goals({4, 0, 2, 2});
    const NewGoal timed = new_goal(0, {AYNI_TRIGGER_INTERVAL, 30, 1'000'000}, 500);
    goals.created(1, timed, 1, goal_state(AYNI_GOAL_ACTIVE, 3, 2, true));
    goals.created(2, input_goal("button.top"), 1);
    goals.created(3, new_goal(0, {AYNI_TRIGGER_DELAY, 30, 0}), 1,
                  goal_state(AYNI_GOAL_ACTIVE, 1, 0, true));

    const auto timed_state = [](ayni_goal_lifecycle_t lifecycle, uint32_t revision,
                                uint32_t taken) {
        return goal_state(lifecycle, revision, taken, ayni::work::GoalTable::is_live(lifecycle));
    };
    GoalState proposed = timed_state(AYNI_GOAL_PROPOSED, 3, 2);
    GoalState no_lifecycle = timed_state(ayni_goal_lifecycle_t{0}, 3, 2);
    GoalState beyond = timed_state(ayni_goal_lifecycle_t{8}, 3, 2);
    GoalState older = timed_state(AYNI_GOAL_ACTIVE, 2, 2);
    GoalState fewer_taken = timed_state(AYNI_GOAL_ACTIVE, 3, 1);
    GoalState cancel_after = timed_state(AYNI_GOAL_CANCELLED, 4, 2);
    cancel_after.cancel_revision = 5;
    GoalState cancelled_without = timed_state(AYNI_GOAL_CANCELLED, 4, 2);
    GoalState cancel_revision_without = timed_state(AYNI_GOAL_ACTIVE, 4, 2);
    cancel_revision_without.cancel_revision = 4;
    GoalState ended_scheduled = timed_state(AYNI_GOAL_SATISFIED, 4, 2);
    ended_scheduled.next_scheduled = true;
    GoalState ended_expiring = timed_state(AYNI_GOAL_EXPIRED, 4, 2);
    ended_expiring.expiry_pending = true;
    GoalState timed_waiting = timed_state(AYNI_GOAL_ACTIVE, 3, 2);
    timed_waiting.waiting = 1;
    const struct {
        const char *what;
        uint64_t goal_id;
        GoalState state;
    } cases[] = {
        {"proposed", 1, proposed},
        {"no lifecycle", 1, no_lifecycle},
        {"undefined lifecycle", 1, beyond},
        {"older revision", 1, older},
        {"fewer occurrences taken", 1, fewer_taken},
        {"cancel revision after the revision", 1, cancel_after},
        {"cancelled without a cancel revision", 1, cancelled_without},
        {"cancel revision without a cancel", 1, cancel_revision_without},
        {"terminal with a next occurrence", 1, ended_scheduled},
        {"terminal with an expiry", 1, ended_expiring},
        {"time trigger with waiting inputs", 1, timed_waiting},
        {"input scheduled without waiting", 2, goal_state(AYNI_GOAL_ACTIVE, 1, 0, true)},
        {"input waiting without schedule", 2,
         [] {
             GoalState state = goal_state(AYNI_GOAL_ACTIVE, 1);
             state.waiting = 2;
             return state;
         }()},
        {"expiry without one", 3,
         [] {
             GoalState state = goal_state(AYNI_GOAL_ACTIVE, 1, 0, true);
             state.expiry_pending = true;
             return state;
         }()},
    };
    for (const auto &item : cases) {
        const Goal before = goals.goal(item.goal_id);
        EXPECT_EQ(goals.state(item.goal_id, item.state, 5), GoalRestore::corrupt) << item.what;
        const Goal &after = goals.goal(item.goal_id);
        EXPECT_EQ(after.lifecycle, before.lifecycle) << item.what;
        EXPECT_EQ(after.revision, before.revision) << item.what;
        EXPECT_EQ(after.cursor.taken, before.cursor.taken) << item.what;
        EXPECT_EQ(after.cursor.waiting, before.cursor.waiting) << item.what;
        EXPECT_EQ(after.commit_seq, before.commit_seq) << item.what;
    }
    // The same goal with its expiry pending is fine.
    GoalState expiring = timed_state(AYNI_GOAL_ACTIVE, 3, 2);
    expiring.expiry_pending = true;
    EXPECT_EQ(goals.state(1, expiring, 5), GoalRestore::restored);
}

// A terminal goal never changes again; a goal without a trigger has no
// occurrence, and an input goal's occurrences fit their counter. A state
// against either rule is corrupt and changes nothing.
TEST(GoalRestore, restore_state_refuses_a_change_to_a_terminal_goal_or_an_impossible_cursor) {
    Goals goals({4, 0, 2, 2});
    goals.created(1, new_goal(0, {AYNI_TRIGGER_DELAY, 30, 0}), 1,
                  goal_state(AYNI_GOAL_ACTIVE, 1, 0, true));
    GoalState cancelled = goal_state(AYNI_GOAL_CANCELLED, 2);
    cancelled.cancel_revision = 2;
    ASSERT_EQ(goals.state(1, cancelled, 2), GoalRestore::restored);
    EXPECT_EQ(goals.state(1, goal_state(AYNI_GOAL_ACTIVE, 3, 0, true), 3), GoalRestore::corrupt);
    EXPECT_EQ(goals.state(1, cancelled, 3), GoalRestore::corrupt);
    EXPECT_EQ(goals.goal(1).lifecycle, AYNI_GOAL_CANCELLED);
    EXPECT_EQ(goals.goal(1).commit_seq, 2U);

    goals.created(2);
    EXPECT_EQ(goals.state(2, goal_state(AYNI_GOAL_ACTIVE, 1, 0, true), 3), GoalRestore::corrupt);
    EXPECT_FALSE(goals.goal(2).cursor.has_next());

    goals.created(3, input_goal("button.top"), 1);
    GoalState overflowing = goal_state(AYNI_GOAL_ACTIVE, 1, UINT32_MAX, true);
    overflowing.waiting = 1;
    EXPECT_EQ(goals.state(3, overflowing, 3), GoalRestore::corrupt);
    overflowing.taken = UINT32_MAX - 1U;
    EXPECT_EQ(goals.state(3, overflowing, 3), GoalRestore::restored);
}

// ADR-0026 decision 7: goal types are registered anew each boot. A goal
// that is not terminal and whose type this boot lacks, by id or by version,
// is held for that reason in memory, its revision unchanged; a terminal
// goal is never held.
TEST(GoalRestore, resolve_types_holds_live_goals_whose_type_this_boot_lacks) {
    Goals goals({4, 0, 2, 2});
    const auto typed = [](const char *type, uint32_t version) {
        NewGoal goal = new_goal();
        goal.goal_type_id = catalog_id(type);
        goal.goal_type_version = version;
        return goal;
    };
    goals.created(1, typed("kept", 1));
    goals.created(2, typed("gone", 1));
    goals.created(3, typed("kept", 2), 1, goal_state(AYNI_GOAL_PAUSED, 2));
    goals.created(4, typed("gone", 1));
    goals.cancel(4, 2);
    const auto resolve = [](bool all_known) {
        return [all_known](const CatalogId &id, uint32_t version, ayni_trigger_kind_t,
                           uint32_t &out_index) {
            if (all_known || (id.equals(key("kept")) && version == 1U)) {
                out_index = 7;
                return AYNI_REASON_NONE;
            }
            return id.equals(key("kept")) ? AYNI_REASON_GOAL_TYPE_VERSION
                                          : AYNI_REASON_GOAL_TYPE_UNKNOWN;
        };
    };
    goals->resolve_types(resolve(false));

    EXPECT_EQ(goals.goal(1).goal_type_index, 7U);
    EXPECT_EQ(goals.goal(1).hold_reason, AYNI_REASON_NONE);
    EXPECT_FALSE(GoalTable::is_held(goals.goal(1)));
    EXPECT_EQ(goals.goal(2).goal_type_index, GoalTable::no_index);
    EXPECT_EQ(goals.goal(2).hold_reason, AYNI_REASON_GOAL_TYPE_UNKNOWN);
    EXPECT_TRUE(GoalTable::is_held(goals.goal(2)));
    EXPECT_EQ(goals.goal(3).goal_type_index, GoalTable::no_index);
    EXPECT_EQ(goals.goal(3).hold_reason, AYNI_REASON_GOAL_TYPE_VERSION);
    EXPECT_EQ(goals.goal(3).lifecycle, AYNI_GOAL_PAUSED);
    EXPECT_EQ(goals.goal(3).revision, 2U);
    EXPECT_EQ(goals.goal(4).goal_type_index, GoalTable::no_index);
    EXPECT_EQ(goals.goal(4).hold_reason, AYNI_REASON_NONE);
    EXPECT_EQ(goals.goal(2).revision, 1U);
    EXPECT_EQ(goals.goal(2).commit_seq, 1U);

    // Each call resolves anew.
    goals->resolve_types(resolve(true));
    for (const uint64_t goal_id : {1U, 2U, 3U, 4U}) {
        EXPECT_EQ(goals.goal(goal_id).goal_type_index, 7U) << goal_id;
        EXPECT_EQ(goals.goal(goal_id).hold_reason, AYNI_REASON_NONE) << goal_id;
    }
}

// ADR-0026 decision 6: times are on one boot's clock, so every wait
// restarts in full from the recovery time R: a pending expiry at
// R + expiry_after_us, a waiting local input at R, an interval at R plus its
// delay before the first occurrence and its period after, any other trigger
// at R plus its delay.
TEST(GoalRestore, reanchor_restarts_every_wait_in_full_at_recovery) {
    constexpr uint64_t recovery = 50'000;
    Goals goals({8, 0, 2, 2});
    GoalState expiring = goal_state(AYNI_GOAL_ACTIVE, 1, 0, true);
    expiring.expiry_pending = true;
    goals.created(1, new_goal(0, {AYNI_TRIGGER_DELAY, 100, 0}, 500), 1, expiring);
    goals.created(2, new_goal(0, {AYNI_TRIGGER_ONCE, 0, 0}), 1,
                  goal_state(AYNI_GOAL_ACTIVE, 1, 0, true));
    goals.created(3, new_goal(0, {AYNI_TRIGGER_INTERVAL, 30, 1'000'000}), 1,
                  goal_state(AYNI_GOAL_ACTIVE, 1, 0, true));
    goals.created(4, new_goal(0, {AYNI_TRIGGER_INTERVAL, 30, 1'000'000}), 1,
                  goal_state(AYNI_GOAL_PAUSED, 2, 2, true));
    GoalState waiting = goal_state(AYNI_GOAL_ACTIVE, 1, 1, true);
    waiting.waiting = 2;
    goals.created(5, input_goal("button.top"), 1, waiting);
    GoalState expiring_only = goal_state(AYNI_GOAL_ACTIVE, 1);
    expiring_only.expiry_pending = true;
    goals.created(6, new_goal(0, no_trigger, 700), 1, expiring_only);
    goals.created(7, new_goal(0, {AYNI_TRIGGER_DELAY, 100, 0}, 500), 1,
                  goal_state(AYNI_GOAL_ACTIVE, 1, 1, false));
    goals.created(8, new_goal(0, {AYNI_TRIGGER_DELAY, 100, 0}, 500), 1,
                  goal_state(AYNI_GOAL_SATISFIED, 2, 1));
    goals->reanchor(recovery);

    EXPECT_EQ(goals.goal(1).cursor.next_due_us, recovery + 100U);
    EXPECT_EQ(goals.goal(1).expires_us, recovery + 500U);
    EXPECT_EQ(goals.goal(2).cursor.next_due_us, recovery);
    EXPECT_EQ(goals.goal(3).cursor.next_due_us, recovery + 30U);
    EXPECT_EQ(goals.goal(4).cursor.next_due_us, recovery + 1'000'000U);
    EXPECT_EQ(goals.goal(5).cursor.next_due_us, recovery);
    EXPECT_EQ(goals.goal(5).cursor.latest_due_us, recovery);
    EXPECT_EQ(goals.goal(5).cursor.waiting, 2U);
    EXPECT_EQ(goals.goal(6).cursor.next_due_us, no_time);
    EXPECT_EQ(goals.goal(6).expires_us, recovery + 700U);
    EXPECT_EQ(goals.goal(7).cursor.next_due_us, no_time);
    EXPECT_EQ(goals.goal(7).expires_us, no_time);
    EXPECT_EQ(goals.goal(8).cursor.next_due_us, no_time);
    EXPECT_EQ(goals.goal(8).expires_us, no_time);
    for (const uint64_t goal_id : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) {
        EXPECT_EQ(goals.goal(goal_id).commit_seq, 1U) << goal_id;
    }

    // A time past the clock's range saturates and never comes.
    goals->reanchor(no_time - 10U);
    EXPECT_EQ(goals.goal(1).cursor.next_due_us, no_time);
    EXPECT_EQ(goals.goal(1).expires_us, no_time);
    EXPECT_EQ(goals.goal(2).cursor.next_due_us, no_time - 10U);
}

// A recovery that failed leaves no goal it replayed.
TEST(GoalRestore, clear_empties_the_goal_table) {
    Goals goals({2, 0, 2, 2});
    goals.created(1);
    goals.created(2, new_goal(1));
    goals->clear();
    EXPECT_EQ(goals->size(), 0U);
    EXPECT_FALSE(goals.exists(1));
    EXPECT_EQ(goals.restore(2), GoalRestore::restored);
    EXPECT_EQ(goals.last_index(), 0U);
}

// ---- Tasks ----------------------------------------------------------------

using TaskRestore = TaskTable::Restore;

constexpr uint64_t goal_id = 70;

NewTask new_task(uint32_t occurrence = 3, uint32_t occurrence_count = 2) {
    return {goal_id, owner, 900, 4, occurrence, occurrence_count};
}

TaskState task_state(ayni_task_lifecycle_t lifecycle, uint32_t revision, uint32_t step_count = 0) {
    TaskState state{};
    state.lifecycle = lifecycle;
    state.revision = revision;
    state.cancel_revision = lifecycle == AYNI_TASK_CANCELLED ? revision : 0U;
    state.hold_status = AYNI_OK;
    state.hold_reason = AYNI_REASON_NONE;
    state.step_count = step_count;
    return state;
}

TaskState held_state(ayni_task_lifecycle_t lifecycle, uint32_t revision) {
    TaskState state = task_state(lifecycle, revision);
    state.hold_status = AYNI_UNSUPPORTED;
    state.hold_reason = AYNI_REASON_POLICY_NO_PROGRESS;
    return state;
}

class Tasks {
  public:
    explicit Tasks(uint32_t capacity, uint32_t step_capacity = 2, uint32_t checkpoint_max_bytes = 4)
        : limits_{capacity, step_capacity, checkpoint_max_bytes},
          storage_(TaskTable::storage_size(limits_) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), limits_) {}

    TaskTable *operator->() { return &table_; }

    TaskRestore restore(uint64_t task_id, uint64_t replaced_task_id = 0, uint64_t seq = 1,
                        const NewTask &task = new_task()) {
        return table_.restore(task, task_id, replaced_task_id, stamp(seq));
    }
    TaskRestore state(uint64_t task_id, const TaskState &state, uint64_t seq = 2) {
        return table_.restore_state(index_of(task_id), state, stamp(seq));
    }
    TaskRestore step(uint64_t task_id, uint32_t position, ayni_string_v1 step_key,
                     uint64_t seq = 2) {
        return table_.restore_step(index_of(task_id), position, step_key, stamp(seq));
    }
    TaskRestore checkpoint(uint64_t task_id, ayni_string_v1 writer, uint32_t writer_version,
                           const std::vector<uint8_t> &state, uint64_t seq = 2) {
        return table_.restore_checkpoint(index_of(task_id), writer, writer_version,
                                         {state.data(), static_cast<uint32_t>(state.size())},
                                         stamp(seq));
    }
    // Ends the task by the TASK_STATE of a cancel.
    void cancel(uint64_t task_id, uint64_t seq) {
        const Task &ended = task(task_id);
        ASSERT_EQ(state(task_id,
                        task_state(AYNI_TASK_CANCELLED, ended.revision + 1U, ended.step_count),
                        seq),
                  TaskRestore::restored);
    }

    bool exists(uint64_t task_id) const {
        uint32_t index = 0;
        return table_.find(task_id, index);
    }
    uint32_t index_of(uint64_t task_id) const {
        uint32_t index = 0;
        EXPECT_TRUE(table_.find(task_id, index)) << task_id;
        return index;
    }
    const Task &task(uint64_t task_id) const { return table_.at(index_of(task_id)); }

    std::vector<std::string> steps(uint64_t task_id) const {
        const uint32_t index = index_of(task_id);
        std::vector<std::string> keys;
        for (uint32_t step = 0; step < table_.at(index).step_count; ++step) {
            const CatalogId &id = table_.step(index, step);
            keys.emplace_back(id.data(), id.size());
        }
        return keys;
    }
    std::vector<uint8_t> checkpoint_of(uint64_t task_id) const {
        const ayni_bytes_v1 state = table_.checkpoint(index_of(task_id));
        return {state.data, state.data + state.size};
    }

  private:
    TaskTable::Limits limits_;
    std::vector<uint64_t> storage_;
    TaskTable table_;
};

// ADR-0026: a TASK record replays the task as insert() created it in the
// record's stamp: READY at revision 1, with no steps, checkpoint or hold.
TEST(TaskRestore, restore_creates_the_task_as_insert_did_in_its_stamp) {
    Tasks tasks(2);
    ASSERT_EQ(tasks.restore(1, 0, 6), TaskRestore::restored);
    const Task &task = tasks.task(1);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(task.revision, 1U);
    EXPECT_EQ(task.cancel_revision, 0U);
    EXPECT_EQ(task.commit_seq, 6U);
    EXPECT_EQ(task.created_transition_id, 106U);
    EXPECT_EQ(task.replaced_task_id, 0U);
    EXPECT_EQ(task.goal_id, goal_id);
    EXPECT_EQ(task.owner_instance_id, owner);
    EXPECT_EQ(task.goal_index, 4U);
    EXPECT_EQ(task.occurrence, 3U);
    EXPECT_EQ(task.occurrence_count, 2U);
    EXPECT_EQ(task.step_count, 0U);
    EXPECT_EQ(task.checkpoint_writer_version, 0U);
    EXPECT_EQ(task.hold_status, AYNI_OK);
    EXPECT_FALSE(tasks->has_held());
}

// ADR-0025: a task that took an ended task's place takes its slot again and
// starts with no steps and an empty checkpoint, also on a boot with more
// room; the other tasks keep theirs.
TEST(TaskRestore, restore_takes_the_slot_of_the_task_it_replaced) {
    for (const uint32_t capacity : {2U, 3U}) {
        Tasks tasks(capacity, 2, 4);
        ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
        ASSERT_EQ(tasks.restore(2), TaskRestore::restored);
        ASSERT_EQ(tasks.step(1, 0, key("a")), TaskRestore::restored);
        ASSERT_EQ(tasks.checkpoint(1, key("plan"), 1, {1, 1}), TaskRestore::restored);
        ASSERT_EQ(tasks.step(2, 0, key("b")), TaskRestore::restored);
        ASSERT_EQ(tasks.checkpoint(2, key("plan"), 1, {2}), TaskRestore::restored);
        tasks.cancel(1, 3);
        const uint32_t slot = tasks.task(1).slot;

        ASSERT_EQ(tasks.restore(3, 1, 4), TaskRestore::restored) << capacity;
        EXPECT_FALSE(tasks.exists(1));
        EXPECT_EQ(tasks->size(), 2U);
        EXPECT_EQ(tasks.task(3).slot, slot);
        EXPECT_EQ(tasks.task(3).replaced_task_id, 1U);
        EXPECT_TRUE(tasks.steps(3).empty());
        EXPECT_TRUE(tasks.checkpoint_of(3).empty());
        EXPECT_EQ(tasks.steps(2), (std::vector<std::string>{"b"}));
        EXPECT_EQ(tasks.checkpoint_of(2), (std::vector<uint8_t>{2}));
    }
}

// A TASK record no transition writes is corrupt; one this boot has no room
// for is no_room; neither changes anything.
TEST(TaskRestore, restore_refuses_a_task_no_transition_creates) {
    Tasks tasks(2);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.restore(2), TaskRestore::restored);
    const struct {
        const char *what;
        uint64_t task_id;
        uint64_t replaced_task_id;
        NewTask task;
    } cases[] = {
        {"no id", 0, 0, new_task()},
        {"no occurrence", 9, 0, new_task(0, 1)},
        {"standing for no occurrence", 9, 0, new_task(3, 0)},
        {"more occurrences than numbers", 9, 0, new_task(2, 3)},
        {"id taken", 1, 0, new_task()},
        {"unknown replaced task", 9, 99, new_task()},
        {"live replaced task", 9, 1, new_task()},
    };
    for (const auto &item : cases) {
        EXPECT_EQ(tasks.restore(item.task_id, item.replaced_task_id, 3, item.task),
                  TaskRestore::corrupt)
            << item.what;
    }
    EXPECT_EQ(tasks.restore(9, 0, 3, new_task(2, 2)), TaskRestore::no_room);
    EXPECT_EQ(tasks->size(), 2U);
    EXPECT_FALSE(tasks.exists(9));
    EXPECT_EQ(tasks.task(1).lifecycle, AYNI_TASK_READY);
}

// ADR-0026: a TASK_STATE record replays what a change left, in its stamp;
// a decision's hold counts as held until the task ends.
TEST(TaskRestore, restore_state_replays_what_a_change_left) {
    Tasks tasks(3);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.restore(2), TaskRestore::restored);
    ASSERT_EQ(tasks.state(1, task_state(AYNI_TASK_PAUSED, 2), 4), TaskRestore::restored);
    EXPECT_EQ(tasks.task(1).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(tasks.task(1).revision, 2U);
    EXPECT_EQ(tasks.task(1).commit_seq, 4U);
    EXPECT_FALSE(tasks->has_held());
    ASSERT_EQ(tasks.state(1, task_state(AYNI_TASK_CANCELLED, 3), 5), TaskRestore::restored);
    EXPECT_EQ(tasks.task(1).cancel_revision, 3U);

    ASSERT_EQ(tasks.state(2, held_state(AYNI_TASK_PAUSED, 2), 6), TaskRestore::restored);
    EXPECT_EQ(tasks.task(2).hold_status, AYNI_UNSUPPORTED);
    EXPECT_EQ(tasks.task(2).hold_reason, AYNI_REASON_POLICY_NO_PROGRESS);
    EXPECT_TRUE(TaskTable::still_held(tasks.task(2)));
    EXPECT_TRUE(tasks->has_held());
    ASSERT_EQ(tasks.state(2, held_state(AYNI_TASK_FAILED, 3), 8), TaskRestore::restored);
    EXPECT_EQ(tasks.task(2).hold_status, AYNI_UNSUPPORTED);
    EXPECT_FALSE(tasks->has_held());
}

// An ended task never changes again, and a held one takes no change but an
// end, so a later live state of either is corrupt and changes nothing.
TEST(TaskRestore, restore_state_refuses_a_change_to_an_ended_or_held_task) {
    Tasks tasks(2);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.restore(2), TaskRestore::restored);
    ASSERT_EQ(tasks.state(1, task_state(AYNI_TASK_CANCELLED, 2), 4), TaskRestore::restored);
    ASSERT_EQ(tasks.state(2, held_state(AYNI_TASK_PAUSED, 2), 5), TaskRestore::restored);

    EXPECT_EQ(tasks.state(1, task_state(AYNI_TASK_READY, 3), 6), TaskRestore::corrupt);
    EXPECT_EQ(tasks.state(1, task_state(AYNI_TASK_CANCELLED, 3), 6), TaskRestore::corrupt);
    EXPECT_EQ(tasks.task(1).lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(tasks.task(1).commit_seq, 4U);
    EXPECT_EQ(tasks.state(2, held_state(AYNI_TASK_PAUSED, 3), 6), TaskRestore::corrupt);
    EXPECT_EQ(tasks.state(2, task_state(AYNI_TASK_READY, 3), 6), TaskRestore::corrupt);
    EXPECT_EQ(tasks.task(2).revision, 2U);
    EXPECT_TRUE(tasks->has_held());
}

// A TASK_STATE record no transition writes is corrupt and changes nothing:
// a lifecycle this library does not bring a task to, an older revision,
// steps the replay did not see, a cancel revision that does not match, a
// hold reason without a failure, a held task READY.
TEST(TaskRestore, restore_state_refuses_a_state_no_transition_writes) {
    Tasks tasks(1);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.step(1, 0, key("a")), TaskRestore::restored);
    ASSERT_EQ(tasks.state(1, task_state(AYNI_TASK_READY, 3, 1)), TaskRestore::restored);

    TaskState cancel_after = task_state(AYNI_TASK_CANCELLED, 4, 1);
    cancel_after.cancel_revision = 5;
    TaskState cancelled_without = task_state(AYNI_TASK_CANCELLED, 4, 1);
    cancelled_without.cancel_revision = 0;
    TaskState cancel_revision_without = task_state(AYNI_TASK_PAUSED, 4, 1);
    cancel_revision_without.cancel_revision = 4;
    TaskState reason_without_status = task_state(AYNI_TASK_PAUSED, 4, 1);
    reason_without_status.hold_reason = AYNI_REASON_POLICY_NO_PROGRESS;
    TaskState held_ready = held_state(AYNI_TASK_READY, 4);
    held_ready.step_count = 1;
    const struct {
        const char *what;
        TaskState state;
    } cases[] = {
        {"running", task_state(AYNI_TASK_RUNNING, 4, 1)},
        {"cancelling", task_state(AYNI_TASK_CANCELLING, 4, 1)},
        {"no lifecycle", task_state(ayni_task_lifecycle_t{0}, 4, 1)},
        {"undefined lifecycle", task_state(ayni_task_lifecycle_t{9}, 4, 1)},
        {"older revision", task_state(AYNI_TASK_READY, 2, 1)},
        {"fewer steps", task_state(AYNI_TASK_READY, 4, 0)},
        {"more steps", task_state(AYNI_TASK_READY, 4, 2)},
        {"cancel revision after the revision", cancel_after},
        {"cancelled without a cancel revision", cancelled_without},
        {"cancel revision without a cancel", cancel_revision_without},
        {"hold reason without a failure", reason_without_status},
        {"held and ready", held_ready},
    };
    for (const auto &item : cases) {
        EXPECT_EQ(tasks.state(1, item.state, 5), TaskRestore::corrupt) << item.what;
        EXPECT_EQ(tasks.task(1).lifecycle, AYNI_TASK_READY) << item.what;
        EXPECT_EQ(tasks.task(1).revision, 3U) << item.what;
        EXPECT_EQ(tasks.task(1).commit_seq, 2U) << item.what;
        EXPECT_EQ(tasks.task(1).hold_status, AYNI_OK) << item.what;
    }
    EXPECT_FALSE(tasks->has_held());
}

// ADR-0024: a TASK_STEPS record replays each step at its position, in the
// transition that added it, before that transition's TASK_STATE.
TEST(TaskRestore, restore_step_replays_each_step_at_its_position) {
    Tasks tasks(1, 3);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.step(1, 0, key("turn"), 2), TaskRestore::restored);
    ASSERT_EQ(tasks.step(1, 1, key("photo"), 2), TaskRestore::restored);
    EXPECT_EQ(tasks.task(1).steps_seq, 2U);
    EXPECT_EQ(tasks.task(1).steps_before, 0U);
    ASSERT_EQ(tasks.state(1, task_state(AYNI_TASK_READY, 2, 2), 2), TaskRestore::restored);
    ASSERT_EQ(tasks.step(1, 2, key(long_id), 3), TaskRestore::restored);
    EXPECT_EQ(tasks.task(1).steps_seq, 3U);
    EXPECT_EQ(tasks.task(1).steps_before, 2U);
    ASSERT_EQ(tasks.state(1, task_state(AYNI_TASK_COMPLETED, 3, 3), 3), TaskRestore::restored);
    EXPECT_EQ(tasks.steps(1), (std::vector<std::string>{"turn", "photo", long_id}));
}

// Steps are unique catalog ids, only ever appended, and bounded by this
// boot's step capacity (no_room).
TEST(TaskRestore, restore_step_refuses_gaps_repeats_and_invalid_keys) {
    Tasks tasks(1, 2);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.step(1, 0, key("a")), TaskRestore::restored);
    EXPECT_EQ(tasks.step(1, 0, key("b")), TaskRestore::corrupt);
    EXPECT_EQ(tasks.step(1, 2, key("b")), TaskRestore::corrupt);
    EXPECT_EQ(tasks.step(1, 1, key("a")), TaskRestore::corrupt);
    EXPECT_EQ(tasks.step(1, 1, key("B")), TaskRestore::corrupt);
    EXPECT_EQ(tasks.step(1, 1, key("")), TaskRestore::corrupt);
    EXPECT_EQ(tasks.step(1, 1, key("abcdefghijklmnopqrstuvwxyz.012345")), TaskRestore::corrupt);
    EXPECT_EQ(tasks.steps(1), (std::vector<std::string>{"a"}));
    ASSERT_EQ(tasks.step(1, 1, key("b")), TaskRestore::restored);
    EXPECT_EQ(tasks.step(1, 2, key("c")), TaskRestore::no_room);
    EXPECT_EQ(tasks.steps(1), (std::vector<std::string>{"a", "b"}));
}

// ADR-0024, C-POLICY-010: a TASK_CHECKPOINT record replays the private
// checkpoint with the strategy and version that wrote it.
TEST(TaskRestore, restore_checkpoint_replays_the_private_checkpoint) {
    Tasks tasks(1, 2, 4);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.checkpoint(1, key(long_id), 3, {1, 2, 3, 4}, 5), TaskRestore::restored);
    EXPECT_TRUE(tasks.task(1).checkpoint_writer_id.equals(key(long_id)));
    EXPECT_EQ(tasks.task(1).checkpoint_writer_version, 3U);
    EXPECT_EQ(tasks.task(1).checkpoint_seq, 5U);
    EXPECT_EQ(tasks.checkpoint_of(1), (std::vector<uint8_t>{1, 2, 3, 4}));
    ASSERT_EQ(tasks.checkpoint(1, key("plan"), 4, {}, 6), TaskRestore::restored);
    EXPECT_TRUE(tasks.checkpoint_of(1).empty());
    EXPECT_EQ(tasks.task(1).checkpoint_writer_version, 4U);
    EXPECT_EQ(tasks.task(1).checkpoint_seq, 6U);
}

// A checkpoint has a writer, a strategy id at a nonzero version; one larger
// than this boot's checkpoint_max_bytes is no_room. Neither changes
// anything.
TEST(TaskRestore, restore_checkpoint_refuses_one_no_strategy_wrote) {
    Tasks tasks(1, 2, 4);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.checkpoint(1, key("plan"), 2, {9}), TaskRestore::restored);
    EXPECT_EQ(tasks.checkpoint(1, key("plan"), 0, {1}), TaskRestore::corrupt);
    for (const char *writer : {"", "Plan", "pl an", "abcdefghijklmnopqrstuvwxyz.012345"}) {
        EXPECT_EQ(tasks.checkpoint(1, key(writer), 3, {1}), TaskRestore::corrupt) << writer;
    }
    EXPECT_EQ(tasks.checkpoint(1, key("plan"), 3, {1, 2, 3, 4, 5}), TaskRestore::no_room);
    EXPECT_EQ(tasks.task(1).checkpoint_writer_version, 2U);
    EXPECT_EQ(tasks.checkpoint_of(1), (std::vector<uint8_t>{9}));
}

// ADR-0026 decision 7: a live task whose strategy this boot cannot use is
// held PAUSED in memory, at its revision and commit_seq, so the log keeps it
// as it was; a decision's own hold stays, and an ended task is left alone.
TEST(TaskRestore, hold_restored_holds_a_live_task_at_its_revision) {
    Tasks tasks(4);
    for (const uint64_t task_id : {1U, 2U, 3U, 4U}) {
        ASSERT_EQ(tasks.restore(task_id), TaskRestore::restored);
    }
    ASSERT_EQ(tasks.state(2, task_state(AYNI_TASK_PAUSED, 2), 3), TaskRestore::restored);
    ASSERT_EQ(tasks.state(3, held_state(AYNI_TASK_PAUSED, 2), 4), TaskRestore::restored);
    ASSERT_EQ(tasks.state(4, task_state(AYNI_TASK_COMPLETED, 2), 5), TaskRestore::restored);
    for (const uint64_t task_id : {1U, 2U, 3U, 4U}) {
        tasks->hold_restored(tasks.index_of(task_id), AYNI_UNSUPPORTED_VERSION,
                             AYNI_REASON_STRATEGY_CHANGED);
    }

    for (const uint64_t task_id : {1U, 2U}) {
        const Task &task = tasks.task(task_id);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED) << task_id;
        EXPECT_EQ(task.hold_status, AYNI_UNSUPPORTED_VERSION) << task_id;
        EXPECT_EQ(task.hold_reason, AYNI_REASON_STRATEGY_CHANGED) << task_id;
        EXPECT_TRUE(TaskTable::still_held(task)) << task_id;
    }
    EXPECT_EQ(tasks.task(1).revision, 1U);
    EXPECT_EQ(tasks.task(1).commit_seq, 1U);
    EXPECT_EQ(tasks.task(2).revision, 2U);
    EXPECT_EQ(tasks.task(2).commit_seq, 3U);
    EXPECT_EQ(tasks.task(3).hold_status, AYNI_UNSUPPORTED);
    EXPECT_EQ(tasks.task(3).hold_reason, AYNI_REASON_POLICY_NO_PROGRESS);
    EXPECT_EQ(tasks.task(4).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(tasks.task(4).hold_status, AYNI_OK);

    // Three tasks are held, each counted once: ending them one by one
    // releases the count only with the last.
    for (const uint64_t task_id : {1U, 2U, 3U}) {
        EXPECT_TRUE(tasks->has_held()) << task_id;
        tasks->apply(tasks.index_of(task_id), TaskChange::cancel, stamp(6));
    }
    EXPECT_FALSE(tasks->has_held());
}

// ADR-0026 decision 6: a task's latest occurrence fell due on another
// boot's clock, so every task falls due at the recovery time R.
TEST(TaskRestore, reanchor_makes_every_task_due_at_recovery) {
    Tasks tasks(3);
    ASSERT_EQ(tasks.restore(1, 0, 1, {goal_id, owner, 5, 4, 1, 1}), TaskRestore::restored);
    ASSERT_EQ(tasks.restore(2, 0, 1, {goal_id, owner, 9'000'000, 4, 1, 1}), TaskRestore::restored);
    ASSERT_EQ(tasks.restore(3), TaskRestore::restored);
    tasks.cancel(3, 2);
    tasks->reanchor(50'000);
    for (const uint64_t task_id : {1U, 2U, 3U}) {
        EXPECT_EQ(tasks.task(task_id).due_us, 50'000U) << task_id;
    }
}

// A recovery that failed leaves no task it replayed, nor any hold.
TEST(TaskRestore, clear_empties_the_task_table) {
    Tasks tasks(2);
    ASSERT_EQ(tasks.restore(1), TaskRestore::restored);
    ASSERT_EQ(tasks.restore(2), TaskRestore::restored);
    ASSERT_EQ(tasks.state(2, held_state(AYNI_TASK_PAUSED, 2)), TaskRestore::restored);
    ASSERT_TRUE(tasks->has_held());
    tasks->clear();
    EXPECT_EQ(tasks->size(), 0U);
    EXPECT_FALSE(tasks->has_held());
    EXPECT_FALSE(tasks.exists(1));
    ASSERT_EQ(tasks.restore(2), TaskRestore::restored);
    EXPECT_EQ(tasks.task(2).slot, 0U);
    EXPECT_EQ(tasks.task(2).lifecycle, AYNI_TASK_READY);
}

// ---- Replay and journal ---------------------------------------------------

static_assert(AgentTable::storage_alignment <= alignof(uint64_t) &&
                  ayni::catalog::GoalTypeRegistry::storage_alignment <= alignof(uint64_t) &&
                  GoalTable::storage_alignment <= alignof(uint64_t) &&
                  ayni::authority::SessionTable::storage_alignment <= alignof(uint64_t) &&
                  TaskTable::storage_alignment <= alignof(uint64_t) &&
                  ayni::catalog::StrategyRegistry::storage_alignment <= alignof(uint64_t) &&
                  ayni::kernel::CommandTable::storage_alignment <= alignof(uint64_t),
              "vector storage suffices");

// Storage for one store of a work model.
class Block {
  public:
    explicit Block(std::size_t size) : words_(size / sizeof(uint64_t) + 1U) {}
    void *data() { return words_.data(); }

  private:
    std::vector<uint64_t> words_;
};

// Capacities under which one transition can change everything: three
// agents, four goals, three tasks, a decision that adds two steps of the
// two a task holds, an 8-byte checkpoint and two commands per principal.
KernelCapacities small_capacities() {
    KernelCapacities capacities{};
    capacities.agent_capacity = 3;
    capacities.goal_type_capacity = 1;
    capacities.goal_capacity = 4;
    capacities.goal_parameter_max_bytes = 16;
    capacities.session_capacity = 1;
    capacities.task_capacity = 3;
    capacities.strategy_capacity = 1;
    capacities.policy_intent_capacity = 4;
    capacities.task_step_capacity = 2;
    capacities.task_checkpoint_max_bytes = 8;
    capacities.command_capacity = 2;
    return capacities;
}

// A work model in its own storage with the owner registered.
class Model {
  public:
    // with_owner: the instance the goals belong to, as an earlier batch
    // started it.
    explicit Model(const KernelCapacities &capacities, bool with_owner = false)
        : agents_(AgentTable::storage_size(capacities.agent_capacity)),
          goal_types_(ayni::catalog::GoalTypeRegistry::storage_size(capacities.goal_type_capacity)),
          goals_(GoalTable::storage_size(capacities.goal_capacity,
                                         capacities.goal_parameter_max_bytes)),
          sessions_(ayni::authority::SessionTable::storage_size(capacities.session_capacity)),
          tasks_(TaskTable::storage_size({capacities.task_capacity, capacities.task_step_capacity,
                                          capacities.task_checkpoint_max_bytes})),
          strategies_(ayni::catalog::StrategyRegistry::storage_size(capacities.strategy_capacity)),
          commands_(ayni::kernel::CommandTable::storage_size(
              ayni::authority::PrincipalRegistry::capacity, capacities.command_capacity)),
          work_(capacities, {2, 4}, storage()) {
        EXPECT_TRUE(work_.principals.add_owner({principal, 1, AYNI_PRINCIPAL_OWNER}));
        if (with_owner) {
            EXPECT_EQ(work_.agents.restore(owner_agent, owner, 1, AYNI_AGENT_RUNNING, {1, 1}),
                      AgentTable::Restore::restored);
        }
    }

    WorkModel &operator*() { return work_; }
    WorkModel *operator->() { return &work_; }

  private:
    KernelStorage storage() {
        return {nullptr,
                nullptr,
                nullptr,
                agents_.data(),
                goal_types_.data(),
                goals_.data(),
                sessions_.data(),
                tasks_.data(),
                strategies_.data(),
                nullptr,
                commands_.data()};
    }

    Block agents_;
    Block goal_types_;
    Block goals_;
    Block sessions_;
    Block tasks_;
    Block strategies_;
    Block commands_;
    WorkModel work_;
};

// Hands one record to the replay as the batch commit_seq would.
RecordUse feed(WorkReplay &replay, uint64_t commit_seq, uint16_t type,
               const std::vector<uint8_t> &body) {
    const ayni::durability::BatchIdentity batch{1, commit_seq, commit_seq + 100U};
    const RecordHeader header{type, 1, record_required, static_cast<uint32_t>(body.size())};
    return WorkReplay::on_record(&replay, batch, header, body.data());
}

// A top-level goal as created: ACTIVE at revision 1, without a trigger.
Goal created_goal(uint64_t id) {
    Goal goal{};
    goal.goal_id = id;
    goal.owner_instance_id = owner;
    goal.origin = {AYNI_ORIGIN_USER, AYNI_CHANNEL_MANAGEMENT, principal, 1};
    goal.goal_type_id = catalog_id("reminder");
    goal.goal_type_version = 1;
    goal.trigger = no_trigger;
    goal.cursor = {0, no_time};
    goal.expires_us = no_time;
    goal.lifecycle = AYNI_GOAL_ACTIVE;
    goal.revision = 1;
    return goal;
}

std::vector<uint8_t> goal_record(uint64_t id) {
    const Goal goal = created_goal(id);
    std::vector<uint8_t> body(ayni::work::goal_record_size(goal));
    ayni::work::write_goal_record(goal, {nullptr, 0}, body.data());
    return body;
}

std::vector<uint8_t> goal_state_record(uint64_t id) {
    std::vector<uint8_t> body(ayni::work::goal_state_record_size);
    ayni::work::write_goal_state_record(created_goal(id), body.data());
    return body;
}

// A READY task of goal goal_of at revision revision.
Task created_task(uint64_t id, uint64_t goal_of, uint32_t revision = 1) {
    Task task{};
    task.task_id = id;
    task.goal_id = goal_of;
    task.owner_instance_id = owner;
    task.occurrence = 1;
    task.occurrence_count = 1;
    task.revision = revision;
    task.lifecycle = AYNI_TASK_READY;
    task.hold_status = AYNI_OK;
    return task;
}

std::vector<uint8_t> task_record(uint64_t id, uint64_t goal_of) {
    std::vector<uint8_t> body(ayni::work::task_record_size);
    ayni::work::write_task_record(created_task(id, goal_of), body.data());
    return body;
}

std::vector<uint8_t> task_state_record(uint64_t id, uint32_t revision = 1) {
    std::vector<uint8_t> body(ayni::work::task_state_record_size);
    ayni::work::write_task_state_record(created_task(id, 0, revision), body.data());
    return body;
}

// ADR-0026: a transition that creates a goal writes its GOAL_STATE in the
// same batch. Without it the log is corrupt: at the next batch's first
// record, and at the end of the log (complete(), which the kernel turns
// into LOG_CORRUPT); a state of another goal does not stand in for it.
TEST(WorkReplay, goal_created_without_its_state_is_corrupt) {
    {
        Model model(small_capacities(), true);
        WorkReplay replay(*model);
        EXPECT_TRUE(replay.complete());
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_state_record_type, goal_state_record(4)),
                  RecordUse::applied);
        EXPECT_TRUE(replay.complete());
        ASSERT_EQ(feed(replay, 2, ayni::work::goal_record_type, goal_record(5)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 2, ayni::work::goal_state_record_type, goal_state_record(4)),
                  RecordUse::applied);
        EXPECT_FALSE(replay.complete());
        EXPECT_EQ(feed(replay, 3, ayni::work::goal_state_record_type, goal_state_record(5)),
                  RecordUse::corrupt);
    }
    {
        Model model(small_capacities(), true);
        WorkReplay replay(*model);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(5)),
                  RecordUse::applied);
        EXPECT_FALSE(replay.complete());
    }
}

// Likewise a TASK without its TASK_STATE.
TEST(WorkReplay, task_created_without_its_state_is_corrupt) {
    {
        Model model(small_capacities(), true);
        WorkReplay replay(*model);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_state_record_type, goal_state_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 2, ayni::work::task_record_type, task_record(8, 4)),
                  RecordUse::applied);
        EXPECT_FALSE(replay.complete());
        EXPECT_EQ(feed(replay, 3, ayni::work::task_state_record_type, task_state_record(8)),
                  RecordUse::corrupt);
    }
    {
        Model model(small_capacities(), true);
        WorkReplay replay(*model);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_state_record_type, goal_state_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 1, ayni::work::task_record_type, task_record(8, 4)),
                  RecordUse::applied);
        EXPECT_FALSE(replay.complete());
    }
}

// The states in the same batch complete the creations, and later batches
// replay on.
TEST(WorkReplay, state_in_the_same_batch_completes_a_creation) {
    Model model(small_capacities(), true);
    WorkReplay replay(*model);
    ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)), RecordUse::applied);
    ASSERT_EQ(feed(replay, 1, ayni::work::goal_state_record_type, goal_state_record(4)),
              RecordUse::applied);
    ASSERT_EQ(feed(replay, 1, ayni::work::task_record_type, task_record(8, 4)), RecordUse::applied);
    ASSERT_EQ(feed(replay, 1, ayni::work::task_state_record_type, task_state_record(8)),
              RecordUse::applied);
    EXPECT_TRUE(replay.complete());
    EXPECT_EQ(feed(replay, 2, ayni::work::task_state_record_type, task_state_record(8, 2)),
              RecordUse::applied);
    EXPECT_TRUE(replay.complete());
    EXPECT_EQ((*model).tasks.size(), 1U);
    EXPECT_EQ((*model).tasks.at(0).revision, 2U);
}

// A transition creates at most one goal and one task, so a second creation
// before the first one's state is corrupt.
TEST(WorkReplay, second_creation_before_the_first_state_is_corrupt) {
    {
        Model model(small_capacities(), true);
        WorkReplay replay(*model);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)),
                  RecordUse::applied);
        EXPECT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(5)),
                  RecordUse::corrupt);
    }
    {
        Model model(small_capacities(), true);
        WorkReplay replay(*model);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_state_record_type, goal_state_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 1, ayni::work::task_record_type, task_record(8, 4)),
                  RecordUse::applied);
        EXPECT_EQ(feed(replay, 1, ayni::work::task_record_type, task_record(9, 4)),
                  RecordUse::corrupt);
    }
}

// A goal is created for an instance an earlier record started (C-LIFE-020).
TEST(WorkReplay, goal_of_an_instance_no_record_started_is_corrupt) {
    Model model(small_capacities());
    WorkReplay replay(*model);
    EXPECT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)), RecordUse::corrupt);
}

// Ending a goal ends its live tasks in the same transition (ADR-0025
// decision 5), so a log that ends a goal and leaves a task of it live is
// inconsistent, which the kernel turns into LOG_CORRUPT.
TEST(WorkReplay, live_task_under_an_ended_goal_is_inconsistent) {
    Model model(small_capacities(), true);
    WorkReplay replay(*model);
    ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)), RecordUse::applied);
    ASSERT_EQ(feed(replay, 1, ayni::work::goal_state_record_type, goal_state_record(4)),
              RecordUse::applied);
    ASSERT_EQ(feed(replay, 2, ayni::work::task_record_type, task_record(8, 4)), RecordUse::applied);
    ASSERT_EQ(feed(replay, 2, ayni::work::task_state_record_type, task_state_record(8)),
              RecordUse::applied);
    EXPECT_TRUE(replay.consistent());

    Goal cancelled = created_goal(4);
    cancelled.lifecycle = AYNI_GOAL_CANCELLED;
    cancelled.revision = 2;
    cancelled.cancel_revision = 2;
    std::vector<uint8_t> body(ayni::work::goal_state_record_size);
    ayni::work::write_goal_state_record(cancelled, body.data());
    ASSERT_EQ(feed(replay, 3, ayni::work::goal_state_record_type, body), RecordUse::applied);
    EXPECT_TRUE(replay.complete());
    EXPECT_FALSE(replay.consistent());

    // The same transition ending the task as well leaves it consistent.
    Task cancelled_task = created_task(8, 0, 2);
    cancelled_task.lifecycle = AYNI_TASK_CANCELLED;
    cancelled_task.cancel_revision = 2;
    std::vector<uint8_t> task_body(ayni::work::task_state_record_size);
    ayni::work::write_task_state_record(cancelled_task, task_body.data());
    ASSERT_EQ(feed(replay, 3, ayni::work::task_state_record_type, task_body), RecordUse::applied);
    EXPECT_TRUE(replay.consistent());
}

// Ending a goal ends its descendants, and stopping an instance ends its
// goals, in the same transition (ADR-0018), so a live goal under an ended
// parent or a stopped owner is inconsistent too.
TEST(WorkReplay, live_goal_under_an_ended_parent_or_a_stopped_owner_is_inconsistent) {
    {
        Model model(small_capacities(), true);
        WorkReplay replay(*model);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_state_record_type, goal_state_record(4)),
                  RecordUse::applied);
        Goal child = created_goal(5);
        child.parent_goal_id = 4;
        child.depth = 1;
        std::vector<uint8_t> child_body(ayni::work::goal_record_size(child));
        ayni::work::write_goal_record(child, {nullptr, 0}, child_body.data());
        ASSERT_EQ(feed(replay, 2, ayni::work::goal_record_type, child_body), RecordUse::applied);
        ASSERT_EQ(feed(replay, 2, ayni::work::goal_state_record_type, goal_state_record(5)),
                  RecordUse::applied);
        EXPECT_TRUE(replay.consistent());
        Goal cancelled = created_goal(4);
        cancelled.lifecycle = AYNI_GOAL_CANCELLED;
        cancelled.revision = 2;
        cancelled.cancel_revision = 2;
        std::vector<uint8_t> body(ayni::work::goal_state_record_size);
        ayni::work::write_goal_state_record(cancelled, body.data());
        ASSERT_EQ(feed(replay, 3, ayni::work::goal_state_record_type, body), RecordUse::applied);
        EXPECT_FALSE(replay.consistent());
    }
    {
        Model model(small_capacities(), true);
        WorkReplay replay(*model);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_record_type, goal_record(4)),
                  RecordUse::applied);
        ASSERT_EQ(feed(replay, 1, ayni::work::goal_state_record_type, goal_state_record(4)),
                  RecordUse::applied);
        EXPECT_TRUE(replay.consistent());
        ASSERT_EQ((*model).agents.restore(owner_agent, owner, 2, AYNI_AGENT_STOPPED, {102, 2}),
                  AgentTable::Restore::restored);
        EXPECT_FALSE(replay.consistent());
    }
}

constexpr TransitionStamp everything{110, 9};

// Applies one transition, everything, that changes all a transition can
// under small_capacities(): it starts the three instances, pauses three
// goals and creates the fourth with the largest ids and parameters, cancels
// one task, decides another with two steps of the largest keys and the
// largest checkpoint, creates the third task and maps a command. The
// earlier transitions only created the goals and tasks it changes.
void change_everything(WorkModel &work) {
    for (const uint64_t agent_id : {100U, 200U, 300U}) {
        ASSERT_EQ(work.agents.add(agent_id), AgentTable::AddResult::added);
    }
    work.ids.start(1);
    uint32_t index = 0;
    for (const uint64_t id : {1U, 2U, 3U}) {
        ASSERT_NE(work.goals.insert(new_goal(), id, stamp(id), index),
                  GoalTable::Placement::refused);
    }
    ASSERT_TRUE(work.tasks.insert({1, owner, 0, 0, 1, 1}, 11, stamp(4)));
    ASSERT_TRUE(work.tasks.insert({1, owner, 0, 0, 1, 1}, 12, stamp(5)));

    work.agents.start_all(work.ids, everything);
    for (const uint64_t id : {1U, 2U, 3U}) {
        ASSERT_TRUE(work.goals.find(id, index));
        work.goals.apply(index, GoalChange::pause, everything);
    }
    const std::vector<uint8_t> parameters(16, 0x5A);
    NewGoal largest = new_goal(0, {AYNI_TRIGGER_LOCAL_INPUT, 0, 0}, 0,
                               {parameters.data(), static_cast<uint32_t>(parameters.size())});
    largest.goal_type_id = catalog_id(long_id);
    largest.trigger_source = catalog_id(long_id);
    ASSERT_EQ(work.goals.insert(largest, 4, everything, index), GoalTable::Placement::new_place);

    ASSERT_TRUE(work.tasks.find(11, index));
    work.tasks.apply(index, TaskChange::cancel, everything);
    ASSERT_TRUE(work.tasks.find(12, index));
    ASSERT_TRUE(work.tasks.add_step(index, key(long_id), everything));
    ASSERT_TRUE(work.tasks.add_step(index, key("zyxwvutsrqponmlkjihgfedcba.43210"), everything));
    const std::vector<uint8_t> state(8, 0xC3);
    work.tasks.set_checkpoint(index, catalog_id(long_id), 1,
                              {state.data(), static_cast<uint32_t>(state.size())}, everything);
    work.tasks.progress(index, everything);
    ASSERT_TRUE(work.tasks.insert({4, owner, 0, 3, 1, 1}, 13, everything));

    ASSERT_TRUE(work.commands.has_row(principal));
    work.commands.insert({principal,
                          77,
                          0xD1,
                          everything.commit_seq,
                          AYNI_INTENT_GOAL_CREATE,
                          {0, 4, 1, AYNI_OBJECT_GOAL}});
}

// The records of an encoded batch, in order.
struct Encoded {
    BatchView batch;
    std::vector<RecordHeader> headers;
    std::vector<std::vector<uint8_t>> bodies;
};

Encoded decode(const std::vector<uint8_t> &bytes) {
    Encoded encoded{};
    EXPECT_EQ(ayni::durability::read_batch(bytes.data(), bytes.size(), encoded.batch),
              BatchCheck::ok);
    RecordReader reader(encoded.batch);
    RecordHeader header{};
    const uint8_t *body = nullptr;
    while (reader.next(header, body) == RecordReader::Next::record) {
        encoded.headers.push_back(header);
        encoded.bodies.emplace_back(body, body + header.length);
    }
    return encoded;
}

// Encodes the transition into a batch of at most capacity bytes; empty when
// encode_transition() returned 0.
std::vector<uint8_t> encode(const WorkModel &work, TransitionStamp transition,
                            std::size_t capacity) {
    std::vector<uint8_t> bytes(capacity);
    BatchWriter writer(bytes.data(), bytes.size(),
                       {1, transition.commit_seq, transition.transition_id});
    bytes.resize(ayni::kernel::encode_transition(work, transition, writer));
    return bytes;
}

uint64_t id_in(const std::vector<uint8_t> &body) {
    return ayni::foundation::load_le64(body.data());
}

// ADR-0026: a transition's batch holds the records of what it created or
// changed, each REQUIRED and at version 1, in replay order: instances, the
// goal created, the goals' states, the task created, the steps and
// checkpoint of the task decided, the tasks' states, then the command.
TEST(WorkJournal, transition_writes_its_records_in_replay_order) {
    Model model(small_capacities());
    change_everything(*model);
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    const std::vector<uint8_t> bytes =
        encode(*model, everything, ayni::kernel::transition_batch_max(small_capacities()));
    ASSERT_FALSE(bytes.empty());
    const Encoded encoded = decode(bytes);
    EXPECT_EQ(encoded.batch.identity.commit_seq, everything.commit_seq);
    EXPECT_EQ(encoded.batch.identity.transition_id, everything.transition_id);

    const struct {
        uint16_t type;
        uint64_t id; // the first field of the body
    } expected[] = {
        {ayni::work::instance_record_type, 100},
        {ayni::work::instance_record_type, 200},
        {ayni::work::instance_record_type, 300},
        {ayni::work::goal_record_type, 4},
        {ayni::work::goal_state_record_type, 1},
        {ayni::work::goal_state_record_type, 2},
        {ayni::work::goal_state_record_type, 3},
        {ayni::work::goal_state_record_type, 4},
        {ayni::work::task_record_type, 13},
        {ayni::work::task_steps_record_type, 12},
        {ayni::work::task_checkpoint_record_type, 12},
        {ayni::work::task_state_record_type, 11},
        {ayni::work::task_state_record_type, 12},
        {ayni::work::task_state_record_type, 13},
        {ayni::kernel::command_record_type, principal},
    };
    ASSERT_EQ(encoded.headers.size(), sizeof(expected) / sizeof(expected[0]));
    for (std::size_t record = 0; record < encoded.headers.size(); ++record) {
        EXPECT_EQ(encoded.headers[record].type, expected[record].type) << record;
        EXPECT_EQ(encoded.headers[record].version, 1U) << record;
        EXPECT_EQ(encoded.headers[record].flags, record_required) << record;
        EXPECT_EQ(id_in(encoded.bodies[record]), expected[record].id) << record;
    }
    CommandMapping mapping{};
    uint64_t floor = 0;
    ASSERT_TRUE(ayni::kernel::read_command_record(
        encoded.bodies.back().data(), static_cast<uint32_t>(encoded.bodies.back().size()), mapping,
        floor));
    EXPECT_EQ(mapping.command_id, 77U);
}

// Records are found by the stamp: a later transition writes only what it
// changed, and one that changed nothing writes an empty batch.
TEST(WorkJournal, transition_writes_only_what_it_changed) {
    Model model(small_capacities());
    change_everything(*model);
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    const TransitionStamp later{111, 10};
    uint32_t index = 0;
    ASSERT_TRUE((*model).goals.find(4, index));
    (*model).goals.apply(index, GoalChange::pause, later);
    const std::size_t capacity = ayni::kernel::transition_batch_max(small_capacities());

    const Encoded paused = decode(encode(*model, later, capacity));
    ASSERT_EQ(paused.headers.size(), 1U);
    EXPECT_EQ(paused.headers[0].type, ayni::work::goal_state_record_type);
    EXPECT_EQ(id_in(paused.bodies[0]), 4U);

    const Encoded unchanged = decode(encode(*model, {112, 11}, capacity));
    EXPECT_EQ(unchanged.batch.record_count, 0U);
}

// The bound a storage buffer is sized by: the transition that changes the
// most under the capacities fills transition_batch_max() exactly, and one
// byte less does not fit, which encode_transition() reports as 0. A decision
// adds at most policy_intent_capacity steps.
TEST(WorkJournal, largest_transition_fills_transition_batch_max_exactly) {
    const KernelCapacities capacities = small_capacities();
    Model model(capacities);
    change_everything(*model);
    ASSERT_FALSE(testing::Test::HasFatalFailure());
    const uint32_t batch_max = ayni::kernel::transition_batch_max(capacities);
    EXPECT_EQ(encode(*model, everything, batch_max).size(), batch_max);
    EXPECT_TRUE(encode(*model, everything, batch_max - 1U).empty());

    const uint32_t buffer_min = ayni::kernel::storage_buffer_min(capacities);
    EXPECT_EQ(buffer_min, batch_max + ayni::durability::JournalRing::entry_overhead);
    std::vector<uint8_t> buffer(buffer_min);
    ayni::durability::JournalRing ring(buffer.data(), buffer_min);
    EXPECT_TRUE(ring.fits(batch_max));

    KernelCapacities fewer_intents = capacities;
    fewer_intents.policy_intent_capacity = 1;
    EXPECT_EQ(ayni::kernel::transition_batch_max(fewer_intents),
              batch_max - ayni::durability::string_field_size(CatalogId::max_size));
}

} // namespace
