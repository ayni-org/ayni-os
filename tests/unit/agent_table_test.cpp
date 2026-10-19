// Agents and their instances owned by work/agent (contracts/domain/
// lifecycles.md AgentInstance, C-ID-010).

#include "foundation/kernel_id.h"
#include "work/agent/agent_table.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using ayni::foundation::IdAllocator;
using ayni::work::AgentInstance;
using ayni::work::AgentTable;

class Agents {
  public:
    explicit Agents(uint32_t capacity)
        : storage_(AgentTable::storage_size(capacity) / sizeof(uint64_t) + 1U),
          table_(storage_.data(), capacity) {}
    AgentTable *operator->() { return &table_; }

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
    EXPECT_FALSE(agents->has_registered());
    EXPECT_EQ(agents->find_instance(first->instance_id), first);
}

// A registered agent has no instance yet, so instance id 0 names nothing.
TEST(AgentTable, instance_id_0_names_no_instance) {
    Agents agents(1);
    ASSERT_EQ(agents->add(100), AgentTable::AddResult::added);
    EXPECT_EQ(agents->find_instance(0), nullptr);
}

} // namespace
