#include "work/task/task_authority_records.h"

#include "foundation/record_body.h"
namespace ayni::work {

void write_task_authority_record(const TaskAuthorityRecord &record, uint8_t *body) {
    foundation::BodyWriter writer(body, task_authority_record_max(record.count));
    writer.u64(record.task_id);
    writer.u64(record.ceiling.goal_id);
    writer.u64(record.ceiling.commit_seq);
    writer.u64(record.stamp.transition_id);
    writer.u64(record.stamp.commit_seq);
    writer.u32(static_cast<uint32_t>(record.phase));
    writer.u32(record.count);
    for (uint32_t index = 0; index < record.count; ++index) {
        writer.u64(record.references[index].grant_id);
        writer.u32(record.references[index].revision);
    }
}

// Parse locally before the shared check; an invalid later field must not
// publish a valid prefix. No live directory or Grant state is consulted.
bool read_task_authority_record(const uint8_t *body, uint32_t size, TaskAuthorityRecord &out) {
    if (body == nullptr || size < task_authority_record_fixed_size) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    TaskAuthorityRecord record{};
    record.task_id = reader.u64();
    record.ceiling = {reader.u64(), reader.u64()};
    record.stamp = {reader.u64(), reader.u64()};
    record.phase = static_cast<TaskAuthorityPhase>(reader.u32());
    record.count = reader.u32();
    if (record.count > AYNI_GOAL_REQUIREMENT_CAPACITY_MAX) {
        return false;
    }
    for (uint32_t index = 0; index < record.count; ++index) {
        record.references[index] = {reader.u64(), reader.u32()};
    }
    if (!reader.ok() || !valid_task_authority_record(record)) {
        return false;
    }
    out = record;
    return true;
}

} // namespace ayni::work
