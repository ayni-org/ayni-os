// Recovery of the work model from the log (ADR-0026). The boot sequence
// hands every record it does not own to on_record() as it reads the log, in
// log order, so the tables pass through the state each stored transition
// left. A record that breaks the rules of its table is corruption, and one
// the capacities of this boot cannot hold is no_room; either ends recovery
// in RESTRICTED, and the kernel then discards everything replayed.
//
// Once the log is read and this boot's epoch is durable, finish() fits what
// came back to this boot: goal types, strategies and agents are registered
// anew each boot, so a goal whose type is gone, a task whose checkpoint its
// strategy cannot read and an instance whose agent is gone are held or
// FAULTED in memory only, and the log keeps them as they were for a boot
// that registers them again. Times are on one boot's clock, so each wait
// starts again in full when recovery ends.

#ifndef AYNI_CORE_KERNEL_WORK_REPLAY_H
#define AYNI_CORE_KERNEL_WORK_REPLAY_H

#include "durability/batch/prefix_scan.h"
#include "kernel/intent/work_model.h"

#include <cstdint>

namespace ayni::kernel {

class WorkReplay {
  public:
    // model outlives the replay.
    explicit WorkReplay(WorkModel &model) : model_(model) {}

    WorkReplay(const WorkReplay &) = delete;
    WorkReplay &operator=(const WorkReplay &) = delete;

    // A durability::RecordHandler; context is the replay.
    static durability::RecordUse on_record(void *context, const durability::BatchIdentity &batch,
                                           const durability::RecordHeader &header,
                                           const uint8_t *body);
    // Whether on_record() takes records of this type and version; any other
    // record is unknown to it.
    static bool knows(const durability::RecordHeader &header);

    // After the last batch: whether every goal and task a batch created came
    // with its state in the same batch.
    bool complete() const { return pending_goal_id_ == 0U && pending_task_id_ == 0U; }
    // After the last batch: whether the tables hold a state a transition
    // leaves, beyond what each record's own checks cover.
    bool consistent() const;
    // Only once complete() and consistent(): holds and faults what this boot
    // lacks, then puts every wait on the clock at now_us.
    void finish(uint64_t now_us);
    // Drops everything replayed, as when recovery fails.
    void discard();
    // Reload retains registration, but no session or cached due-work from
    // the discarded boot may reach the recovered model.
    void discard_for_reload();

  private:
    durability::RecordUse apply(const durability::BatchIdentity &batch,
                                const durability::RecordHeader &header, const uint8_t *body);
    durability::RecordUse restore_instance(const uint8_t *body, uint32_t size,
                                           foundation::TransitionStamp stamp);
    durability::RecordUse restore_goal(const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp);
    durability::RecordUse restore_goal_state(const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    durability::RecordUse restore_task(const uint8_t *body, uint32_t size,
                                       foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_state(const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_steps(const uint8_t *body, uint32_t size,
                                             foundation::TransitionStamp stamp);
    durability::RecordUse restore_task_checkpoint(const uint8_t *body, uint32_t size,
                                                  foundation::TransitionStamp stamp);
    durability::RecordUse restore_command(const uint8_t *body, uint32_t size, uint64_t commit_seq);
    void hold_tasks();

    WorkModel &model_;
    uint64_t batch_seq_ = 0; // of the batch the last record came in
    // The goal and the task the batch created whose state has not come yet.
    uint64_t pending_goal_id_ = 0;
    uint64_t pending_task_id_ = 0;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_WORK_REPLAY_H
