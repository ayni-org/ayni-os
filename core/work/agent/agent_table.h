// Agents and their resident instances (contracts/domain/lifecycles.md
// AgentInstance). Each registered agent has one instance on this kernel. The
// instance is created when the kernel first enters RUNNING, because its id
// embeds the durable boot epoch (C-ID-010); until the log stores instances
// (MA-010), each boot creates them again with new ids. STARTING takes no time
// until strategies have a start step, so start_all goes straight to RUNNING,
// and STOPPING has nothing to wait for until tasks run, so stop goes straight
// to STOPPED (ADR-0018). Kernel shutdown does not change instance lifecycles.

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
    uint32_t revision;                 // 1 once started, raised by each change
    ayni_agent_lifecycle_t lifecycle;
};

// Pause takes RUNNING to PAUSED and resume takes it back; stop takes RUNNING
// or PAUSED to STOPPED, which has no exit.
enum class InstanceChange { pause, resume, stop };

class AgentTable {
  public:
    enum class AddResult { added, duplicate, full };

    // Why a change is refused, in the order check() checks.
    enum class ChangeCheck {
        allowed,
        revision_conflict,
        invalid_state,
        revision_exhausted,
    };

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

    // The checks of a change a caller asked for by revision.
    ChangeCheck check(uint32_t index, InstanceChange change, uint32_t expected_revision) const;
    // Only after check() returned allowed, with no change in between. The
    // instance gets the next revision and stamp.
    void apply(uint32_t index, InstanceChange change, foundation::TransitionStamp stamp);

    const AgentInstance *find_agent(uint64_t agent_id) const;
    // Null for 0, which no instance has.
    const AgentInstance *find_instance(uint64_t instance_id) const;
    // Returns false for 0 and for an id no instance has.
    bool find_instance(uint64_t instance_id, uint32_t &out_index) const;

    // index < size().
    const AgentInstance &at(uint32_t index) const { return agents_[index]; }
    uint32_t size() const { return agents_.size(); }

  private:
    foundation::BoundedArray<AgentInstance> agents_;
};

} // namespace ayni::work

#endif // AYNI_CORE_WORK_AGENT_AGENT_TABLE_H
