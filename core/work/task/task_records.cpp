#include "work/task/task_records.h"

namespace ayni::work {

// Calendar and source markers are current facts, not version-selected extensions.
void write_task_record(const Task &task, uint8_t *body, bool provenance) {
    durability::BodyWriter writer(body, task_record_size);
    writer.u64(task.task_id);
    writer.u64(task.goal_id);
    writer.u64(task.owner_instance_id);
    writer.u64(task.replaced_task_id);
    writer.u32(task.occurrence);
    writer.u32(task.occurrence_count);
    const bool calendar = task.calendar_local_s != AYNI_CALENDAR_TIME_NONE;
    writer.u64(calendar ? static_cast<uint64_t>(task.calendar_local_s) : 0);
    writer.u64(calendar ? static_cast<uint64_t>(task.calendar_utc_s) : 0);
    writer.u32(calendar ? 1U : 0U);
    writer.u32(provenance ? 1U : 0U);
}

bool read_task_record(const uint8_t *body, uint32_t size, TaskRecord &out) {
    if (body == nullptr || size != task_record_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    TaskRecord record{};
    record.task_id = reader.u64();
    record.task.goal_id = reader.u64();
    record.task.owner_instance_id = reader.u64();
    record.replaced_task_id = reader.u64();
    record.task.occurrence = reader.u32();
    record.task.occurrence_count = reader.u32();
    const auto local = reader.u64();
    const auto utc = reader.u64();
    const auto calendar = reader.u32();
    const auto provenance = reader.u32();
    if (calendar > 1 || provenance > 1 ||
        (calendar == 0 ? local != 0 || utc != 0
                       : local > static_cast<uint64_t>(AYNI_CALENDAR_SECONDS_MAX) ||
                             utc > static_cast<uint64_t>(AYNI_CALENDAR_SECONDS_MAX))) {
        return false;
    }
    if (calendar != 0) {
        record.task.calendar_local_s = static_cast<int64_t>(local);
        record.task.calendar_utc_s = static_cast<int64_t>(utc);
    }
    record.provenance_required = provenance != 0;
    if (!reader.ok()) {
        return false;
    }
    out = record;
    return true;
}

void write_task_snapshot_record(const Task &task, uint8_t *body, bool provenance) {
    write_task_record(task, body, provenance);
    durability::BodyWriter writer(body + task_record_size, task_snapshot_tail_size);
    writer.u64(task.created_transition_id);
    writer.u64(task.commit_seq);
    writer.u64(task.steps_seq);
    writer.u64(task.checkpoint_seq);
    writer.u32(task.steps_before);
}

bool read_task_snapshot_record(const uint8_t *body, uint32_t size, TaskSnapshotRecord &out) {
    if (body == nullptr || size != task_snapshot_record_size ||
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

// One full state carries runtime/call/wait facts plus a separate transition proof.
void write_task_state_record(const Task &task, uint8_t *body, TaskRecordChange change) {
    durability::BodyWriter writer(body, task_state_record_size);
    writer.u64(task.task_id);
    writer.u32(task.lifecycle);
    writer.u32(task.revision);
    writer.u32(task.cancel_revision);
    writer.u32(static_cast<uint32_t>(task.hold_status));
    writer.u32(task.hold_reason);
    writer.u32(task.step_count);
    writer.u32(task.model_calls.used);
    writer.u32(task.model_calls.limit);
    writer.u32(task.model_calls.blocked ? 1U : 0U);
    writer.u64(task.call.id);
    writer.u32(static_cast<uint32_t>(task.call.end));
    const auto wait = task.subscription_wait.value_or(SubscriptionWait{});
    writer.u64(wait.subscription_id);
    writer.u64(wait.generation);
    writer.u64(wait.sequence);
    writer.u32(task.subscription_wait ? static_cast<uint32_t>(wait.phase) : 0);
    writer.u32(static_cast<uint32_t>(change));
    writer.u32(task.subscription_wait ? 1U : 0U);
}

// Validate present and absent states together before publishing the parsed view.
bool read_task_state_record(const uint8_t *body, uint32_t size, TaskStateRecord &out) {
    if (body == nullptr || size != task_state_record_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    TaskStateRecord record{};
    record.task_id = reader.u64();
    record.state.lifecycle = reader.u32();
    record.state.revision = reader.u32();
    record.state.cancel_revision = reader.u32();
    record.state.hold_status = static_cast<ayni_status_t>(reader.u32());
    record.state.hold_reason = reader.u32();
    record.state.step_count = reader.u32();
    record.state.model_calls.used = reader.u32();
    record.state.model_calls.limit = reader.u32();
    const auto blocked = reader.u32();
    record.state.model_calls.blocked = blocked != 0;
    record.state.call.id = reader.u64();
    record.state.call.end = static_cast<TaskCall::End>(reader.u32());
    SubscriptionWait wait{};
    wait.subscription_id = reader.u64();
    wait.generation = reader.u64();
    wait.sequence = reader.u64();
    wait.phase = static_cast<SubscriptionPhase>(reader.u32());
    const auto change = reader.u32();
    const auto present = reader.u32();
    if (blocked > 1 || present > 1 || change < static_cast<uint32_t>(TaskRecordChange::state) ||
        change > static_cast<uint32_t>(TaskRecordChange::resume_wait) ||
        (present == 0 && (wait.subscription_id != 0 || wait.generation != 0 || wait.sequence != 0 ||
                          static_cast<uint32_t>(wait.phase) != 0))) {
        return false;
    }
    if (present != 0) {
        record.state.subscription_wait = wait;
    }
    record.change = static_cast<TaskRecordChange>(change);
    if (!reader.ok() || !record.state.model_calls.valid() ||
        !record.state.call.valid(record.state.lifecycle) ||
        !valid_subscription_wait(record.state.subscription_wait, record.state.lifecycle,
                                 record.state.call.id)) {
        return false;
    }
    out = record;
    return true;
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

void write_task_checkpoint_record(const Task &task, ayni_bytes_v1 state, uint8_t *body,
                                  bool provenance) {
    durability::BodyWriter writer(body, task_checkpoint_record_size(task));
    writer.u64(task.task_id);
    writer.u32(task.checkpoint_writer_version);
    writer.bytes(task.checkpoint_writer_id.data(), task.checkpoint_writer_id.size());
    writer.bytes(state.data, state.size);
    writer.u32(provenance ? 1U : 0U);
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
    const auto provenance = reader.u32();
    out.provenance_required = provenance != 0;
    return reader.ok() && provenance <= 1;
}

} // namespace ayni::work
