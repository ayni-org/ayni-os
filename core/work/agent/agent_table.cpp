#include "work/agent/agent_table.h"

#include <limits>

namespace ayni::work {
namespace {

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();

bool allows(const AgentInstance &agent, InstanceChange change) {
    switch (change) {
    case InstanceChange::pause:
        return agent.lifecycle == AYNI_AGENT_RUNNING;
    case InstanceChange::resume:
        return agent.lifecycle == AYNI_AGENT_PAUSED;
    case InstanceChange::stop:
        return agent.lifecycle == AYNI_AGENT_RUNNING || agent.lifecycle == AYNI_AGENT_PAUSED;
    }
    return false;
}

} // namespace

AgentTable::AddResult AgentTable::add(uint64_t agent_id) {
    if (find_agent(agent_id) != nullptr) {
        return AddResult::duplicate;
    }
    const AgentInstance agent{agent_id, 0, {0, 0}, 0, AYNI_AGENT_REGISTERED};
    return agents_.push_back(agent) ? AddResult::added : AddResult::full;
}

bool AgentTable::has_registered() const {
    for (uint32_t index = 0; index < agents_.size(); ++index) {
        if (agents_[index].lifecycle == AYNI_AGENT_REGISTERED) {
            return true;
        }
    }
    return false;
}

void AgentTable::start_all(foundation::IdAllocator &ids, foundation::TransitionStamp stamp) {
    for (uint32_t index = 0; index < agents_.size(); ++index) {
        AgentInstance &agent = agents_[index];
        if (agent.lifecycle != AYNI_AGENT_REGISTERED) {
            continue;
        }
        // Cannot fail; see start_all in the header.
        static_cast<void>(ids.allocate(agent.instance_id));
        agent.stamp = stamp;
        agent.revision = 1;
        agent.lifecycle = AYNI_AGENT_RUNNING;
    }
}

AgentTable::ChangeCheck AgentTable::check(uint32_t index, InstanceChange change,
                                          uint32_t expected_revision) const {
    const AgentInstance &agent = agents_[index];
    if (expected_revision != agent.revision) {
        return ChangeCheck::revision_conflict;
    }
    if (!allows(agent, change)) {
        return ChangeCheck::invalid_state;
    }
    if (agent.revision == max_revision) {
        return ChangeCheck::revision_exhausted;
    }
    return ChangeCheck::allowed;
}

void AgentTable::apply(uint32_t index, InstanceChange change, foundation::TransitionStamp stamp) {
    AgentInstance &agent = agents_[index];
    switch (change) {
    case InstanceChange::pause:
        agent.lifecycle = AYNI_AGENT_PAUSED;
        break;
    case InstanceChange::resume:
        agent.lifecycle = AYNI_AGENT_RUNNING;
        break;
    case InstanceChange::stop:
        agent.lifecycle = AYNI_AGENT_STOPPED;
        break;
    }
    ++agent.revision;
    agent.stamp = stamp;
}

const AgentInstance *AgentTable::find_agent(uint64_t agent_id) const {
    for (uint32_t index = 0; index < agents_.size(); ++index) {
        if (agents_[index].agent_id == agent_id) {
            return &agents_[index];
        }
    }
    return nullptr;
}

const AgentInstance *AgentTable::find_instance(uint64_t instance_id) const {
    uint32_t index = 0;
    return find_instance(instance_id, index) ? &agents_[index] : nullptr;
}

bool AgentTable::find_instance(uint64_t instance_id, uint32_t &out_index) const {
    if (instance_id == 0U) {
        return false;
    }
    for (uint32_t index = 0; index < agents_.size(); ++index) {
        if (agents_[index].instance_id == instance_id) {
            out_index = index;
            return true;
        }
    }
    return false;
}

} // namespace ayni::work
