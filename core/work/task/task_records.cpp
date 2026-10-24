#include "work/task/task_records.h"

namespace ayni::work {

void write_task_record(const Task &task, uint8_t *body) {
    durability::BodyWriter writer(body, task_record_size);
    writer.u64(task.task_id);
    writer.u64(task.goal_id);
    writer.u64(task.owner_instance_id);
    writer.u64(task.replaced_task_id);
    writer.u32(task.occurrence);
    writer.u32(task.occurrence_count);
}

bool read_task_record(const uint8_t *body, uint32_t size, TaskRecord &out) {
    durability::BodyReader reader(body, size);
    out = TaskRecord{};
    out.task_id = reader.u64();
    out.task.goal_id = reader.u64();
    out.task.owner_instance_id = reader.u64();
    out.replaced_task_id = reader.u64();
    out.task.occurrence = reader.u32();
    out.task.occurrence_count = reader.u32();
    return reader.ok();
}

// Snapshot TASK extends the journal creation body with durable history;
// byte layout belongs here, while restoration policy remains with its owner.
void write_task_snapshot_record(const Task &task, uint8_t *body) {
    write_task_record(task, body);
    durability::BodyWriter writer(body + task_record_size, task_snapshot_tail_size);
    writer.u64(task.created_transition_id);
    writer.u64(task.commit_seq);
    writer.u64(task.steps_seq);
    writer.u64(task.checkpoint_seq);
    writer.u32(task.steps_before);
}

bool read_task_snapshot_record(const uint8_t *body, uint32_t size, TaskSnapshotRecord &out) {
    if (size != task_snapshot_record_size ||
        !read_task_record(body, task_record_size, out.creation)) {
        return false;
    }
    durability::BodyReader reader(body + task_record_size, task_snapshot_tail_size);
    out.stamp = {reader.u64(), reader.u64()};
    out.steps_seq = reader.u64();
    out.checkpoint_seq = reader.u64();
    out.steps_before = reader.u32();
    return reader.ok();
}

void write_task_state_record(const Task &task, uint8_t *body) {
    durability::BodyWriter writer(body, task_state_record_size);
    writer.u64(task.task_id);
    writer.u32(task.lifecycle);
    writer.u32(task.revision);
    writer.u32(task.cancel_revision);
    writer.u32(static_cast<uint32_t>(task.hold_status));
    writer.u32(task.hold_reason);
    writer.u32(task.step_count);
}

bool read_task_state_record(const uint8_t *body, uint32_t size, TaskStateRecord &out) {
    durability::BodyReader reader(body, size);
    out.task_id = reader.u64();
    out.state.lifecycle = reader.u32();
    out.state.revision = reader.u32();
    out.state.cancel_revision = reader.u32();
    out.state.hold_status = static_cast<ayni_status_t>(reader.u32());
    out.state.hold_reason = reader.u32();
    out.state.step_count = reader.u32();
    return reader.ok();
}

uint32_t task_steps_record_size(const TaskTable &tasks, uint32_t index, TaskStepRange range) {
    uint32_t size = task_steps_record_fixed_size;
    for (uint32_t step = range.first; step < range.first + range.count; ++step) {
        size += durability::string_field_size(tasks.step(index, step).size());
    }
    return size;
}

void write_task_steps_record(const TaskTable &tasks, uint32_t index, TaskStepRange range,
                             uint8_t *body) {
    const Task &task = tasks.at(index);
    durability::BodyWriter writer(body, task_steps_record_size(tasks, index, range));
    writer.u64(task.task_id);
    writer.u32(range.first);
    writer.u32(range.count);
    for (uint32_t step = range.first; step < range.first + range.count; ++step) {
        const catalog::CatalogId &key = tasks.step(index, step);
        writer.bytes(key.data(), key.size());
    }
}

uint32_t task_checkpoint_record_size(const Task &task) {
    return task_checkpoint_record_fixed_size + task.checkpoint_writer_id.size() +
           task.checkpoint_size;
}

void write_task_checkpoint_record(const Task &task, ayni_bytes_v1 state, uint8_t *body) {
    durability::BodyWriter writer(body, task_checkpoint_record_size(task));
    writer.u64(task.task_id);
    writer.u32(task.checkpoint_writer_version);
    writer.bytes(task.checkpoint_writer_id.data(), task.checkpoint_writer_id.size());
    writer.bytes(state.data, state.size);
}

bool read_task_checkpoint_record(const uint8_t *body, uint32_t size, TaskCheckpointRecord &out) {
    durability::BodyReader reader(body, size);
    out.task_id = reader.u64();
    out.writer_version = reader.u32();
    uint32_t id_size = 0;
    const uint8_t *id = reader.bytes(catalog::CatalogId::max_size, id_size);
    out.writer_id = {reinterpret_cast<const char *>(id), id_size};
    uint32_t state_size = 0;
    const uint8_t *state = reader.bytes(size, state_size);
    out.state = {state, state_size};
    return reader.ok();
}

} // namespace ayni::work
