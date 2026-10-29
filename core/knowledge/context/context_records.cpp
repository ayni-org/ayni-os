#include "knowledge/context/context_records.h"
namespace ayni::knowledge {
void write_context_record(const ContextRecord &r, uint8_t *body) {
    foundation::BodyWriter writer(body, context_record_size(r.dependencies.count));
    writer.u64(r.id);
    writer.u64(r.owner_instance_id);
    writer.u32(r.revision);
    writer.u32(static_cast<uint32_t>(r.pending.action));
    writer.u64(r.generation);
    writer.u64(r.last_turn);
    writer.u32(r.pending.old_revision);
    writer.u32(0); // Reserved, contracts/storage/records.md type26/v1.
    writer.u64(r.pending.turn_id);
    writer.u64(r.pending.task_id);
    writer.u64(r.stamp.transition_id);
    writer.u64(r.stamp.commit_seq);
    write_memory_dependencies(r.dependencies, writer);
}
bool read_context_record(const uint8_t *body, uint32_t size, ContextRecord &out) {
    if (body == nullptr || size < context_record_size(0)) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    ContextRecord record{};
    record.id = reader.u64();
    record.owner_instance_id = reader.u64();
    record.revision = reader.u32();
    record.pending.action = static_cast<ContextAction>(reader.u32());
    record.generation = reader.u64();
    record.last_turn = reader.u64();
    record.pending.old_revision = reader.u32();
    const auto reserved = reader.u32();
    record.pending.turn_id = reader.u64();
    record.pending.task_id = reader.u64();
    record.stamp = {reader.u64(), reader.u64()};
    if (reserved != 0 || !read_memory_dependencies(reader, record.dependencies) || !reader.ok() ||
        !ContextTable::valid(record, memory_dependency_max)) {
        return false;
    }
    out = record;
    return true;
}
} // namespace ayni::knowledge
