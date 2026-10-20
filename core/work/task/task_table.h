// Tasks (contracts/domain/lifecycles.md Task, ADR-0018, ADR-0022). The table
// owns each task's lifecycle, revision, hold and place. A task belongs to one
// goal and keeps that goal's owner for its life (C-LIFE-020). A READY task
// waits for its strategy's decision, which ends it or holds it.
//
// An ended task keeps its place until a new task needs one: then the task
// that ended first gives way, and a task that has not ended never does. A
// place given up moves another task into it, so tasks are found by id and
// an index is valid only until the next insert().

#ifndef AYNI_CORE_WORK_TASK_TASK_TABLE_H
#define AYNI_CORE_WORK_TASK_TASK_TABLE_H

#include "foundation/bounded_array.h"
#include "foundation/transition_stamp.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::work {

// A task the kernel derives from occurrences of a goal.
struct NewTask {
    uint64_t goal_id;
    uint64_t owner_instance_id; // the goal's
    uint64_t due_us;            // when its latest occurrence fell due
    uint32_t goal_index;        // in the goal table
    uint32_t occurrence;        // the number of the latest occurrence it stands for
    uint32_t occurrence_count;  // how many it stands for, at least 1
};

struct Task {
    uint64_t task_id;
    uint64_t goal_id;
    uint64_t owner_instance_id;
    uint64_t created_transition_id;
    uint64_t commit_seq; // of the last change
    uint64_t due_us;
    uint32_t goal_index;
    uint32_t occurrence;
    uint32_t occurrence_count;
    uint32_t revision;        // 1 at creation, raised by each change
    uint32_t cancel_revision; // the revision that cancelled it; 0 otherwise
    ayni_task_lifecycle_t lifecycle;
    // Why its strategy's decision holds it PAUSED, hold_status being a
    // failure; AYNI_OK and AYNI_REASON_NONE if it never did. Kept once the
    // task ends.
    ayni_status_t hold_status;
    ayni_reason_t hold_reason;
};

// Pause takes READY to PAUSED and resume takes it back, unless a decision
// holds it; cancel takes a READY or PAUSED task to CANCELLED, and expire,
// which comes only with its goal's expiry (ADR-0020), to EXPIRED. Complete
// and fail come from its strategy's decision and take a READY task to
// COMPLETED and FAILED (ADR-0022). Tasks do not reach RUNNING or CANCELLING
// yet.
enum class TaskChange { pause, resume, cancel, expire, complete, fail };

class TaskTable {
  public:
    static constexpr uint32_t no_index = 0xFFFFFFFFU; // no task's index

    // Why a change is refused, in the order check() checks.
    enum class ChangeCheck {
        allowed,
        revision_conflict,
        invalid_state,
        revision_exhausted,
    };

    static constexpr std::size_t storage_alignment =
        foundation::BoundedArray<Task>::storage_alignment;
    static constexpr std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<Task>::storage_size(capacity);
    }

    // storage as for BoundedArray<Task>; capacity is at most
    // AYNI_TASK_CAPACITY_MAX.
    TaskTable(void *storage, uint32_t capacity) : tasks_(storage, capacity) {}

    TaskTable(const TaskTable &) = delete;
    TaskTable &operator=(const TaskTable &) = delete;

    // Whether insert() would succeed: a free place, or an ended task to give
    // way.
    bool has_room() const;

    // Creates a READY task at revision 1, giving it the place of the task
    // that ended first if the table is full. Returns false, changing
    // nothing, when has_room() is false.
    bool insert(const NewTask &task, uint64_t task_id, foundation::TransitionStamp stamp);

    // The checks of a change a caller asked for by revision (TASK_CANCEL).
    ChangeCheck check(uint32_t index, TaskChange change, uint32_t expected_revision) const;
    // Only when allows(at(index), change) and the revision is below the
    // largest. The task gets the next revision and stamp.commit_seq.
    void apply(uint32_t index, TaskChange change, foundation::TransitionStamp stamp);

    // Its strategy's decision holds the READY task index, below the largest
    // revision: it becomes PAUSED and no change but an end takes it out.
    // It gets the next revision and stamp.commit_seq. status is a failure.
    void hold(uint32_t index, ayni_status_t status, ayni_reason_t reason,
              foundation::TransitionStamp stamp);

    static bool allows(const Task &task, TaskChange change);
    static bool at_largest_revision(const Task &task);
    // Whether a decision ever held it; still_held() while it has not ended.
    static bool is_held(const Task &task) { return task.hold_status != AYNI_OK; }
    static bool still_held(const Task &task) {
        return task.lifecycle == AYNI_TASK_PAUSED && is_held(task);
    }

    // The task that has been READY longest among those READY since before
    // commit_seq ready_before, by commit_seq and then task id; a task at
    // the largest revision can take no decision and is left out. Returns
    // false when there is none.
    bool next_ready(uint64_t ready_before, uint32_t &out_index) const;
    // Whether some task below the largest revision is READY.
    bool has_ready() const;
    // Whether a task of goal goal_index other than task_index has not ended;
    // with no_index, whether any of its tasks has not ended.
    bool live_task_besides(uint32_t goal_index, uint32_t task_index) const;
    // Whether some task is still_held(), without looking at the tasks.
    bool has_held() const { return held_count_ > 0U; }

    // Returns false when no task in memory has the id.
    bool find(uint64_t task_id, uint32_t &out_index) const;

    // index < size().
    const Task &at(uint32_t index) const { return tasks_[index]; }
    uint32_t size() const { return tasks_.size(); }

    // Not ended: READY, RUNNING, PAUSED or CANCELLING.
    static bool is_live(ayni_task_lifecycle_t lifecycle);

  private:
    foundation::BoundedArray<Task> tasks_;
    uint32_t held_count_ = 0; // still_held() tasks
};

} // namespace ayni::work

#endif // AYNI_CORE_WORK_TASK_TASK_TABLE_H
