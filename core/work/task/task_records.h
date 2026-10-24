// The TASK, TASK_STATE, TASK_STEPS and TASK_CHECKPOINT records
// (contracts/storage/records.md, ADR-0026). A transition that creates a task
// writes its TASK record, what the task was created with; one whose decision
// adds steps or writes the private checkpoint writes the steps it added and
// the checkpoint; and every transition that changes a task, its creation
// included, writes its TASK_STATE record, what the change left, after the
// steps. The stamp of each is its batch's.

#ifndef AYNI_CORE_WORK_TASK_TASK_RECORDS_H
#define AYNI_CORE_WORK_TASK_TASK_RECORDS_H

#include "durability/batch/record_body.h"
#include "work/task/task_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::work {

constexpr uint16_t task_record_type = 5;
constexpr uint16_t task_record_version = 1;
constexpr uint16_t task_state_record_type = 6;
constexpr uint16_t task_state_record_version = 1;
// Same state body, but REQUIRED adjacent Task authority must follow (ADR-0038).
constexpr uint16_t task_state_authority_record_version = 2;
constexpr uint16_t task_steps_record_type = 7;
constexpr uint16_t task_steps_record_version = 1;
constexpr uint16_t task_checkpoint_record_type = 8;
constexpr uint16_t task_checkpoint_record_version = 1;
constexpr uint32_t task_record_size = 40;
constexpr uint32_t task_state_record_size = 32;
// Without its keys.
constexpr uint32_t task_steps_record_fixed_size = 16;
// Without the writer's id and the state.
constexpr uint32_t task_checkpoint_record_fixed_size = 20;

// What a TASK record holds. task.goal_index and task.due_us are left 0.
struct TaskRecord {
    uint64_t task_id;
    uint64_t replaced_task_id;
    NewTask task;
};

struct TaskStateRecord {
    uint64_t task_id;
    TaskState state;
};

// The views point into the body.
struct TaskCheckpointRecord {
    uint64_t task_id;
    uint32_t writer_version;
    ayni_string_v1 writer_id;
    ayni_bytes_v1 state;
};

// body has task_record_size bytes.
void write_task_record(const Task &task, uint8_t *body);
// Returns false for a body of another size.
bool read_task_record(const uint8_t *body, uint32_t size, TaskRecord &out);

// body has task_state_record_size bytes.
void write_task_state_record(const Task &task, uint8_t *body);
bool read_task_state_record(const uint8_t *body, uint32_t size, TaskStateRecord &out);

// The steps of task index added in the transition at.steps_seq.
uint32_t task_steps_record_size(const TaskTable &tasks, uint32_t index);
// body has task_steps_record_size(tasks, index) bytes.
void write_task_steps_record(const TaskTable &tasks, uint32_t index, uint8_t *body);
// Passes each key to on_step(position, key), from position first, while it
// returns true. Returns false when on_step does, or for a body that does
// not hold a TASK_STEPS record; on_step may have seen keys by then.
template <typename OnStep>
bool read_task_steps_record(const uint8_t *body, uint32_t size, uint64_t &out_task_id,
                            OnStep on_step);

uint32_t task_checkpoint_record_size(const Task &task);
// body has task_checkpoint_record_size(task) bytes; state is the task's.
void write_task_checkpoint_record(const Task &task, ayni_bytes_v1 state, uint8_t *body);
bool read_task_checkpoint_record(const uint8_t *body, uint32_t size, TaskCheckpointRecord &out);

template <typename OnStep>
bool read_task_steps_record(const uint8_t *body, uint32_t size, uint64_t &out_task_id,
                            OnStep on_step) {
    durability::BodyReader reader(body, size);
    out_task_id = reader.u64();
    const uint32_t first = reader.u32();
    const uint32_t count = reader.u32();
    if (count == 0U || first > UINT32_MAX - count) {
        return false;
    }
    // Each key takes at least its length, so a count the body cannot hold
    // ends the loop at the body's end.
    for (uint32_t step = 0; step < count; ++step) {
        uint32_t key_size = 0;
        const uint8_t *key = reader.bytes(catalog::CatalogId::max_size, key_size);
        if (key == nullptr) {
            return false;
        }
        if (!on_step(first + step, ayni_string_v1{reinterpret_cast<const char *>(key), key_size})) {
            return false;
        }
    }
    return reader.ok();
}

} // namespace ayni::work

#endif // AYNI_CORE_WORK_TASK_TASK_RECORDS_H
