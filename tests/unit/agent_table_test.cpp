// Agents and their instances owned by work/agent (contracts/domain/
// lifecycles.md AgentInstance, C-ID-010): pause, resume and stop (ADR-0018).

#include "foundation/kernel_id.h"
#include "work/agent/agent_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

namespace {

using ayni::foundation::IdAllocator;
using ayni::work::AgentInstance;
using ayni::work::AgentTable;
using ayni::work::InstanceChange;
using ChangeCheck = AgentTable::ChangeCheck;

class Agents {
  public:
    explicit Agents(uint32_t capacity)
        : storage_(AgentTable::storage_size(capacity) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), capacity) {}
    AgentTable *operator->() { return &table_; }

    // One started agent, at index 0.
    static void start_one(Agents &agents) {
        ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
        IdAllocator ids;
        ids.start(5);
        agents->start_all(ids, {9, 4});
    }

    // Checks and applies; returns what check() said.
    ChangeCheck change(InstanceChange kind, uint64_t seq) {
        const ChangeCheck check = table_.check(0, kind, table_.at(0).revision);
        if (check == ChangeCheck::allowed) {
            table_.apply(0, kind, {seq + 10U, seq});
        }
        return check;
    }

    // White-box: the instances lie at the start of the storage.
    AgentInstance &raw() { return *reinterpret_cast<AgentInstance *>(storage_.data()); }

  private:
    std::vector<uint64_t> storage_;
    AgentTable table_;
};

TEST(AgentTable, registers_each_agent_once_up_to_capacity) {
    Agents agents(2);
    EXPECT_EQ(agents->add(100), AgentTable::AddResult::added);
    EXPECT_EQ(agents->add(100), AgentTable::AddResult::duplicate);
    EXPECT_EQ(agents->add(200), AgentTable::AddResult::added);
    EXPECT_EQ(agents->add(300), AgentTable::AddResult::full);

    const AgentInstance *agent = agents->find_agent(100);
    ASSERT_NE(agent, nullptr);
    EXPECT_EQ(agent->lifecycle, AYNI_AGENT_REGISTERED);
    EXPECT_EQ(agent->instance_id, 0U);
    EXPECT_EQ(agent->revision, 0U);
    EXPECT_EQ(agents->find_agent(300), nullptr);
    EXPECT_TRUE(agents->has_registered());
}

// Instances start together in one transition, each with its own kernel id.
TEST(AgentTable, start_all_gives_each_instance_an_id_and_the_stamp) {
    Agents agents(2);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    ASSERT_EQ(agents->add(200), AgentTable::AddResult::added);
    IdAllocator ids;
    ids.start(5);
    agents->start_all(ids, {9, 4});

    const AgentInstance *first = agents->find_agent(100);
    const AgentInstance *second = agents->find_agent(200);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(first->instance_id, (uint64_t{5} << 32U) | 1U);
    EXPECT_EQ(second->instance_id, (uint64_t{5} << 32U) | 2U);
    EXPECT_EQ(second->stamp.transition_id, 9U);
    EXPECT_EQ(second->stamp.commit_seq, 4U);
    EXPECT_EQ(second->revision, 1U);
    EXPECT_FALSE(agents->has_registered());
    EXPECT_EQ(agents->find_instance(first->instance_id), first);
    uint32_t index = 9;
    ASSERT_TRUE(agents->find_instance(second->instance_id, index));
    EXPECT_EQ(index, 1U);
}

// A registered agent has no instance yet, so instance id 0 names nothing.
TEST(AgentTable, instance_id_0_names_no_instance) {
    Agents agents(1);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    EXPECT_EQ(agents->find_instance(0), nullptr);
    uint32_t index = 9;
    EXPECT_FALSE(agents->find_instance(0, index));
    EXPECT_EQ(index, 9U);
}

TEST(AgentTable, pause_resume_and_stop_follow_the_instance_lifecycle) {
    Agents agents(1);
    Agents::start_one(agents);
    EXPECT_EQ(agents.change(InstanceChange::resume, 5), ChangeCheck::invalid_state);
    ASSERT_EQ(agents.change(InstanceChange::pause, 5), ChangeCheck::allowed);
    EXPECT_EQ(agents->at(0).lifecycle, AYNI_AGENT_PAUSED);
    EXPECT_EQ(agents->at(0).revision, 2U);
    EXPECT_EQ(agents->at(0).stamp.commit_seq, 5U);
    EXPECT_EQ(agents.change(InstanceChange::pause, 6), ChangeCheck::invalid_state);
    ASSERT_EQ(agents.change(InstanceChange::resume, 6), ChangeCheck::allowed);
    EXPECT_EQ(agents->at(0).lifecycle, AYNI_AGENT_RUNNING);
    ASSERT_EQ(agents.change(InstanceChange::pause, 7), ChangeCheck::allowed);

    // A PAUSED instance stops too, and STOPPED has no exit.
    ASSERT_EQ(agents.change(InstanceChange::stop, 8), ChangeCheck::allowed);
    EXPECT_EQ(agents->at(0).lifecycle, AYNI_AGENT_STOPPED);
    EXPECT_EQ(agents->at(0).revision, 5U);
    EXPECT_EQ(agents->at(0).stamp.transition_id, 18U);
    for (const InstanceChange kind :
         {InstanceChange::pause, InstanceChange::resume, InstanceChange::stop}) {
        EXPECT_EQ(agents.change(kind, 9), ChangeCheck::invalid_state);
    }
    EXPECT_EQ(agents->at(0).revision, 5U);
}

TEST(AgentTable, a_registered_agent_has_no_instance_to_change) {
    Agents agents(1);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    EXPECT_EQ(agents->check(0, InstanceChange::pause, 0), ChangeCheck::invalid_state);
    EXPECT_EQ(agents->check(0, InstanceChange::stop, 0), ChangeCheck::invalid_state);
}

// C-COMMIT-012: a stale revision is a conflict before the state is checked,
// and a revision never wraps.
TEST(AgentTable, revision_is_checked_before_the_state_and_never_wraps) {
    Agents agents(1);
    Agents::start_one(agents);
    EXPECT_EQ(agents->check(0, InstanceChange::resume, 2), ChangeCheck::revision_conflict);
    EXPECT_EQ(agents->check(0, InstanceChange::resume, 1), ChangeCheck::invalid_state);
    agents.raw().revision = std::numeric_limits<uint32_t>::max();
    EXPECT_EQ(agents.change(InstanceChange::stop, 5), ChangeCheck::revision_exhausted);
    EXPECT_EQ(agents->at(0).lifecycle, AYNI_AGENT_RUNNING);
}

} // namespace
