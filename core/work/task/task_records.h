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
// Same body; direct live reauthorization preserves state and advances revision once.
constexpr uint16_t task_state_reauthorization_record_version = 3;
// Runtime extension keeps each original pairing meaning (records.md type6).
constexpr uint16_t task_state_runtime_record_version = 4;
constexpr uint16_t task_state_runtime_authority_record_version = 5;
constexpr uint16_t task_state_runtime_reauthorization_record_version = 6;
constexpr uint16_t task_state_raise_limit_record_version = 7;
// Selected-call extension preserves ordinary/authority/reauthorization/raise pairing.
constexpr uint16_t task_state_call_record_version = 8;
constexpr uint16_t task_state_call_authority_record_version = 9;
constexpr uint16_t task_state_call_reauthorization_record_version = 10;
constexpr uint16_t task_state_call_raise_limit_record_version = 11;
// Explicit owner wait resume; same selected-call body, paired with type9/v8.
constexpr uint16_t task_state_resume_wait_record_version = 12;
constexpr uint16_t task_steps_record_type = 7;
constexpr uint16_t task_steps_record_version = 1;
constexpr uint16_t task_checkpoint_record_type = 8;
constexpr uint16_t task_checkpoint_record_version = 1;
// TASK and snapshot TASK layouts: contracts/storage/records.md.
constexpr uint32_t task_record_size = 40;
// created_transition_id, last commit_seq, steps_seq, checkpoint_seq, steps_before.
constexpr uint32_t task_snapshot_tail_size = 36;
constexpr uint32_t task_snapshot_record_size = task_record_size + task_snapshot_tail_size;
constexpr uint32_t task_state_record_size = 32;
constexpr uint32_t task_state_runtime_field_count = 3; // used/limit/blocked, records.md type6/v4+.
constexpr uint32_t task_state_runtime_record_size =
    task_state_record_size + task_state_runtime_field_count * sizeof(uint32_t);
// Selected logical call u64 and requested drain end u32; records.md type6/v8+.
constexpr uint32_t task_state_call_record_size =
    task_state_runtime_record_size + sizeof(uint64_t) + sizeof(uint32_t);
// Legacy defaults need no extension. All other runtime facts require type6/v4+.
uint16_t task_state_version_for(const Task &task, uint16_t base = task_state_record_version);
// Fixed body bytes for the selected known version; excludes the record envelope.
uint32_t task_state_size_for(uint16_t version);
// Pairing meaning of v4/v5/v6, respectively v1/v2/v3; v7 remains explicit raise.
uint16_t task_state_base_version(uint16_t version);
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

// A snapshot TASK's creation fields and durable history. Domain validity is
// checked by TaskTable; snapshot bounds and Goal ownership belong to Kernel.
struct TaskSnapshotRecord {
    TaskRecord creation; // Identity and creation values; no borrowed storage.
    // Original creation transition and last commit sequence; not one change's pair.
    foundation::TransitionStamp stamp;
    uint64_t steps_seq;      // Last step commit sequence; 0 means no steps yet.
    uint64_t checkpoint_seq; // Last checkpoint commit sequence; 0 means none yet.
    uint32_t steps_before;   // Steps before steps_seq; cannot exceed restored step_count.
};

// A contiguous slice of an existing Task's keys. The caller selects a journal
// delta or a single snapshot key; 0 < count <= task.step_count - first.
struct TaskStepRange {
    uint32_t first; // Zero-based key index, strictly below task.step_count.
    uint32_t count; // Number of keys, nonzero; first + count does not overflow.
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

// body has task_snapshot_record_size bytes. Preserves the selected frozen
// Task's original creation/commit/step/checkpoint stamps (ADR-0041).
void write_task_snapshot_record(const Task &task, uint8_t *body);
// Reads exactly that snapshot body; no snapshot-global or cross-table checks.
bool read_task_snapshot_record(const uint8_t *body, uint32_t size, TaskSnapshotRecord &out);

// body has task_state_size_for(version) bytes; caller selects matching version.
void write_task_state_record(const Task &task, uint8_t *body,
                             uint16_t version = task_state_record_version);
bool read_task_state_record(const uint8_t *body, uint32_t size, TaskStateRecord &out,
                            uint16_t version = task_state_record_version);

// Caller provides a valid range in task index; size includes each key length.
uint32_t task_steps_record_size(const TaskTable &tasks, uint32_t index, TaskStepRange range);
// body has task_steps_record_size(tasks, index, range) bytes; same valid range.
void write_task_steps_record(const TaskTable &tasks, uint32_t index, TaskStepRange range,
                             uint8_t *body);
// Largest record for count keys, with count bounded by TaskTable::Limits.
constexpr uint32_t task_steps_record_max(uint32_t count) {
    return task_steps_record_fixed_size +
           count * durability::string_field_size(catalog::CatalogId::max_size);
}
// Passes each key to on_step(position, key), from position first, while it
// returns true. Returns false when on_step does, or for a body that does
// not hold a TASK_STEPS record; on_step may have seen keys by then.
template <typename OnStep>
bool read_task_steps_record(const uint8_t *body, uint32_t size, uint64_t &out_task_id,
                            OnStep on_step);

// Largest checkpoint record for the configured bounded state bytes.
constexpr uint32_t task_checkpoint_record_max(uint32_t state_max_bytes) {
    return task_checkpoint_record_fixed_size + catalog::CatalogId::max_size + state_max_bytes;
}
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
