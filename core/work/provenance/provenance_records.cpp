#include "work/provenance/provenance_records.h"
namespace ayni::work {
void write_provenance_record(const ProvenanceRecord &record, uint8_t *body) {
    foundation::BodyWriter writer(body, provenance_record_size(record));
    writer.u64(record.id);
    writer.u32(static_cast<uint32_t>(record.kind));
    writer.u32(static_cast<uint32_t>(record.content));
    writer.u64(record.stamp.transition_id);
    writer.u64(record.stamp.commit_seq);
    knowledge::write_memory_dependencies(record.dependencies, writer);
    writer.u64(record.origin.id);
    writer.u64(record.origin.generation);
}
bool read_provenance_record(const uint8_t *body, uint32_t size, ProvenanceRecord &out) {
    if (body == nullptr || size < provenance_record_size(0)) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    ProvenanceRecord record{};
    record.id = reader.u64();
    record.kind = static_cast<ProvenanceKind>(reader.u32());
    record.content = static_cast<ContentState>(reader.u32());
    record.stamp.transition_id = reader.u64();
    record.stamp.commit_seq = reader.u64();
    if (!knowledge::read_memory_dependencies(reader, record.dependencies)) {
        return false;
    }
    record.origin = {reader.u64(), reader.u64()};
    if (!reader.ok() || !record.origin.valid()) {
        return false;
    }
    out = record;
    return true;
}
} // namespace ayni::work
