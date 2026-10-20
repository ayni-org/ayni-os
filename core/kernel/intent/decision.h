// Strategies deciding tasks (contracts/host/policy_interface.md, ADR-0022).
// ayni_step has the strategy of each READY task decide it: the kernel hands
// the strategy a snapshot of the task and a buffer for its intents, then
// applies the decision as one transition. A decision ends the task or, when
// the kernel refuses it or the strategy fails, holds it PAUSED with the
// reason. A held task is not decided again, and its goal derives no task,
// until someone cancels the task or the goal expires: asking again would
// mostly repeat the refusal and keep more_ready set.

#ifndef AYNI_CORE_KERNEL_INTENT_DECISION_H
#define AYNI_CORE_KERNEL_INTENT_DECISION_H

#include "kernel/intent/work_model.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

// The kernel memory a strategy writes its intents into
// (ayni_policy_output_v1).
class DecisionBuffer {
  public:
    static constexpr std::size_t storage_alignment = alignof(ayni_policy_intent_v1);
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return sizeof(ayni_policy_intent_v1) * capacity;
    }

    // storage holds storage_size(capacity) bytes aligned to
    // storage_alignment and outlives the buffer.
    DecisionBuffer(void *storage, uint32_t capacity);

    DecisionBuffer(const DecisionBuffer &) = delete;
    DecisionBuffer &operator=(const DecisionBuffer &) = delete;

    uint32_t capacity() const { return capacity_; }

    // Zeroes the first count <= capacity() intents for the next decision,
    // so none is left over from an earlier one.
    ayni_policy_intent_v1 *clear(uint32_t count);
    // index < capacity().
    const ayni_policy_intent_v1 &at(uint32_t index) const { return intents_[index]; }

  private:
    ayni_policy_intent_v1 *intents_;
    uint32_t capacity_;
};

// Has the strategy of the task READY longest among those READY since before
// commit_seq ready_before decide it, as one item and transition; now_us is
// the start of the step. Returns false, calling nothing, when no such task
// is left. Every goal type with tasks names a strategy that ayni_start
// resolved.
bool decide_next(WorkModel &model, DecisionBuffer &decisions, uint64_t ready_before,
                 uint64_t now_us);

// Whether a task waits for a decision; the step reports it as more_ready.
bool awaits_decision(const WorkModel &model);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_INTENT_DECISION_H
