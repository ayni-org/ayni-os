#include "work/agent/agent_table.h"

#include <limits>

namespace ayni::work {
namespace {

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();

bool allows(const AgentInstance &agent, InstanceChange change) {
    const ayni_agent_lifecycle_t lifecycle = AgentTable::effective_lifecycle(agent);
    switch (change) {
    case InstanceChange::pause:
        return lifecycle == AYNI_AGENT_RUNNING;
    case InstanceChange::resume:
        return lifecycle == AYNI_AGENT_PAUSED;
    case InstanceChange::stop:
        return lifecycle == AYNI_AGENT_RUNNING || lifecycle == AYNI_AGENT_PAUSED;
    }
    return false;
}

} // namespace

AgentTable::AddResult AgentTable::add(uint64_t agent_id) {
    if (find_agent(agent_id) != nullptr) {
        return AddResult::duplicate;
    }
    const AgentInstance agent{agent_id,         0,   {0, 0}, 0, AYNI_AGENT_REGISTERED,
                              AYNI_REASON_NONE, true};
    return agents_.push_back(agent) ? AddResult::added : AddResult::full;
}

AgentTable::Restore AgentTable::restore(uint64_t agent_id, uint64_t instance_id, uint32_t revision,
                                        ayni_agent_lifecycle_t lifecycle,
                                        foundation::TransitionStamp stamp) {
    const bool restorable = lifecycle == AYNI_AGENT_RUNNING || lifecycle == AYNI_AGENT_PAUSED ||
                            lifecycle == AYNI_AGENT_STOPPED;
    if (agent_id == 0U || instance_id == 0U || revision == 0U || !restorable) {
        return Restore::corrupt;
    }
    uint32_t holder = 0;
    const bool held = find_instance(instance_id, holder);
    for (uint32_t index = 0; index < agents_.size(); ++index) {
        AgentInstance &agent = agents_[index];
        if (agent.agent_id != agent_id) {
            continue;
        }
        // An agent has one instance for its life, and an id one holder. A
        // stopped instance never changes again, and no change lowers a
        // revision.
        if ((agent.instance_id != 0U &&
             (agent.instance_id != instance_id || agent.lifecycle == AYNI_AGENT_STOPPED ||
              revision < agent.revision)) ||
            (held && holder != index)) {
            return Restore::corrupt;
        }
        agent.instance_id = instance_id;
        agent.stamp = stamp;
        agent.revision = revision;
        agent.lifecycle = lifecycle;
        return Restore::restored;
    }
    if (held) {
        return Restore::corrupt;
    }
    const AgentInstance orphan{agent_id,  instance_id,      stamp, revision,
                               lifecycle, AYNI_REASON_NONE, false};
    return agents_.push_back(orphan) ? Restore::restored : Restore::no_room;
}

void AgentTable::fault_unregistered() {
    for (uint32_t index = 0; index < agents_.size(); ++index) {
        AgentInstance &agent = agents_[index];
        if (!agent.registered && agent.lifecycle != AYNI_AGENT_STOPPED) {
            agent.fault_reason = AYNI_REASON_AGENT_UNREGISTERED;
        }
    }
}

// Registration closed before recovery, so the rows of unregistered agents
// come after every registered one.
void AgentTable::drop_restored() {
    while (agents_.size() > 0U && !agents_[agents_.size() - 1U].registered) {
        agents_.remove_at(agents_.size() - 1U);
    }
    for (uint32_t index = 0; index < agents_.size(); ++index) {
        agents_[index] = {agents_[index].agent_id, 0,   {0, 0}, 0, AYNI_AGENT_REGISTERED,
                          AYNI_REASON_NONE,        true};
    }
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
