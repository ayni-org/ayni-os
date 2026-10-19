#include "work/agent/agent_table.h"

namespace ayni::work {

AgentTable::AddResult AgentTable::add(uint64_t agent_id) {
    if (find_agent(agent_id) != nullptr) {
        return AddResult::duplicate;
    }
    const AgentInstance agent{agent_id, 0, {0, 0}, AYNI_AGENT_REGISTERED};
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
        agent.lifecycle = AYNI_AGENT_RUNNING;
    }
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
    if (instance_id == 0U) {
        return nullptr;
    }
    for (uint32_t index = 0; index < agents_.size(); ++index) {
        if (agents_[index].instance_id == instance_id) {
            return &agents_[index];
        }
    }
    return nullptr;
}

} // namespace ayni::work
