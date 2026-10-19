// Agents and their resident instances (contracts/domain/lifecycles.md
// AgentInstance). Each registered agent has one instance on this kernel. The
// instance is created when the kernel first enters RUNNING, because its id
// embeds the durable boot epoch (C-ID-010); until the log stores instances
// (MA-010), each boot creates them again with new ids. STARTING takes no time
// until strategies have a start step, so start_all goes straight to RUNNING.
// Kernel shutdown does not change instance lifecycles.

#ifndef AYNI_CORE_WORK_AGENT_AGENT_TABLE_H
#define AYNI_CORE_WORK_AGENT_AGENT_TABLE_H

#include "foundation/bounded_array.h"
#include "foundation/kernel_id.h"
#include "foundation/transition_stamp.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::work {

struct AgentInstance {
    uint64_t agent_id;
    uint64_t instance_id;              // 0 while REGISTERED
    foundation::TransitionStamp stamp; // of the last change; 0 while REGISTERED
    ayni_agent_lifecycle_t lifecycle;
};

class AgentTable {
  public:
    enum class AddResult { added, duplicate, full };

    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<AgentInstance>::storage_alignment;
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<AgentInstance>::storage_size(capacity);
    }

    // storage as for BoundedArray<AgentInstance>; capacity is at most
    // AYNI_AGENT_CAPACITY_MAX.
    AgentTable(void *storage, uint32_t capacity) : agents_(storage, capacity) {}

    // Registers an agent; agent_id is nonzero.
    AddResult add(uint64_t agent_id);

    // Whether some agent is still REGISTERED.
    bool has_registered() const;

    // Creates the instance of every REGISTERED agent in one transition: each
    // gets an id from ids and becomes RUNNING. ids has just been started, so
    // it holds far more ids than AYNI_AGENT_CAPACITY_MAX and none runs out.
    void start_all(foundation::IdAllocator &ids, foundation::TransitionStamp stamp);

    const AgentInstance *find_agent(uint64_t agent_id) const;
    // Null for 0, which no instance has.
    const AgentInstance *find_instance(uint64_t instance_id) const;

  private:
    foundation::BoundedArray<AgentInstance> agents_;
};

} // namespace ayni::work

#endif // AYNI_CORE_WORK_AGENT_AGENT_TABLE_H
