// Strategies deciding tasks (contracts/host/policy_interface.md, ADR-0022,
// ADR-0024). ayni_step has the strategy of each READY task decide it: the
// kernel hands the strategy a snapshot of the task, its steps and private
// checkpoint, and buffers for its intents and new private checkpoint, then
// applies the decision as one transition. A decision ends the task, or
// commits steps and leaves it READY to be decided in a later step, or, when
// the kernel refuses it or the strategy fails, holds it PAUSED with the
// reason. Every decision that does not hold the task ends it or commits a
// step, so a task takes a bounded number. A held task is not decided again,
// and its goal derives no task, until the task is cancelled or ends with
// its goal: asking again would mostly repeat the refusal and keep
// more_ready set.

#ifndef AYNI_CORE_KERNEL_DECISION_DECISION_H
#define AYNI_CORE_KERNEL_DECISION_DECISION_H

#include "kernel/service/model/work_model.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

class CallRuntime; // Bounded call coordinator, assembled by Kernel.

// The kernel memory of one decision at a time: the views of the task's
// steps the snapshot shows, the intents the strategy writes and the private
// checkpoint it returns (ayni_task_snapshot_v1, ayni_policy_output_v1).
class DecisionBuffer {
  public:
    struct Limits {
        uint32_t intent_capacity;       // policy_intent_capacity
        uint32_t step_capacity;         // task_step_capacity
        uint32_t checkpoint_max_bytes;  // task_checkpoint_max_bytes
        uint32_t memory_body_bytes = 0; // Separate staged candidate bytes; zero when disabled.
    };

    // The step views, then the intents, then the checkpoint bytes: each
    // needs no more alignment than the one before.
    static constexpr std::size_t storage_alignment = alignof(ayni_task_step_v1);
    static std::size_t storage_size(const Limits &limits);

    // storage holds storage_size(limits) bytes aligned to storage_alignment
    // and outlives the buffer.
    DecisionBuffer(void *storage, const Limits &limits);

    DecisionBuffer(const DecisionBuffer &) = delete;
    DecisionBuffer &operator=(const DecisionBuffer &) = delete;

    uint32_t intent_capacity() const { return intent_capacity_; }

    // Room for step_capacity views.
    ayni_task_step_v1 *steps() { return steps_; }
    // Zeroes the first count <= intent_capacity() intents for the next
    // decision, so none is left over from an earlier one.
    ayni_policy_intent_v1 *clear(uint32_t count);
    // index < intent_capacity().
    const ayni_policy_intent_v1 &at(uint32_t index) const { return intents_[index]; }
    // Copies state into the first capacity bytes of the checkpoint buffer
    // and zeroes the rest of them, so nothing of an earlier decision is
    // left; state.size <= capacity <= checkpoint_max_bytes.
    uint8_t *load_checkpoint(ayni_bytes_v1 state, uint32_t capacity);
    // Candidate staging is independent of the output checkpoint and zeroed after each decision.
    uint8_t *memory_staging() { return memory_staging_; }
    void scrub();
    // The first size <= checkpoint_max_bytes bytes of the buffer.
    ayni_bytes_v1 checkpoint(uint32_t size) const { return {checkpoint_, size}; }

  private:
    ayni_task_step_v1 *steps_;
    ayni_policy_intent_v1 *intents_;
    uint8_t *checkpoint_;
    uint32_t intent_capacity_;
    uint8_t *memory_staging_;   // Arena after checkpoint bytes, null only if memory disabled.
    uint32_t checkpoint_bytes_; // Immutable full scratch span for erasure.
    uint32_t memory_bytes_;     // Immutable candidate span, zero when disabled.
};

// Has the strategy of the task READY longest among those READY since before
// commit_seq ready_before decide it, as one item and transition; now_us is
// the start of the step, and the task's authority is checked at clock's
// reading before and after decide. calls stages, checks and accepts a call
// the strategy proposes. Returns false, calling nothing, when no such task
// is left. Every goal type with tasks names a strategy that ayni_start
// resolved.
enum class DecisionResult {
    none,
    applied,
    internal
}; // No work, one publication, or invariant failure.
DecisionResult decide_next(WorkModel &model, DecisionBuffer &decisions, uint64_t ready_before,
                           uint64_t now_us, uint64_t durable_seq, OwnerClock clock,
                           CallRuntime &calls, uint64_t &accepted_call);

// Whether a task waits for a decision; the step reports it as more_ready.
bool awaits_decision(const WorkModel &model, uint64_t durable_seq);

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_DECISION_DECISION_H
