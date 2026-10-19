// Tasks (contracts/domain/lifecycles.md Task, ADR-0018). The table owns each
// task's lifecycle, revision and place. A task belongs to one goal and keeps
// that goal's owner for its life (C-LIFE-020).
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
};

// Pause takes READY to PAUSED and resume takes it back; cancel takes a READY
// or PAUSED task to CANCELLED. Tasks do not reach RUNNING or CANCELLING yet.
enum class TaskChange { pause, resume, cancel };

class TaskTable {
  public:
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

    static bool allows(const Task &task, TaskChange change);
    static bool at_largest_revision(const Task &task);

    // Returns false when no task in memory has the id.
    bool find(uint64_t task_id, uint32_t &out_index) const;

    // index < size().
    const Task &at(uint32_t index) const { return tasks_[index]; }
    uint32_t size() const { return tasks_.size(); }

    // Not ended: READY, RUNNING, PAUSED or CANCELLING.
    static bool is_live(ayni_task_lifecycle_t lifecycle);

  private:
    foundation::BoundedArray<Task> tasks_;
};

} // namespace ayni::work

#endif // AYNI_CORE_WORK_TASK_TASK_TABLE_H
