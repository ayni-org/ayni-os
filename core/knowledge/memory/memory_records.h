// REQUIRED type 23/v1: memory facts, same body in journal and snapshot (records.md).
#ifndef AYNI_CORE_KNOWLEDGE_MEMORY_RECORDS_H
#define AYNI_CORE_KNOWLEDGE_MEMORY_RECORDS_H
#include "durability/batch/record_types.h"
#include "foundation/record_body.h"
#include "knowledge/memory/memory_types.h"
namespace ayni::knowledge {
constexpr uint16_t memory_record_type = durability::record_type::memory;
constexpr uint16_t memory_record_version = 1;
// records.md: identity/revision/state/stamp, value fixed fields and each exact source reference.
constexpr uint32_t memory_record_header_bytes = 32;
constexpr uint32_t memory_value_fixed_bytes = 48;
constexpr uint32_t memory_dependency_bytes = 52;
// Decoded metadata owns labels/dependencies; body borrows the containing record.
struct DecodedMemory {
    MemoryRecord record; // Complete row facts; table restore validates intrinsic semantics.
    ayni_string_v1 body; // Borrowed UTF-8 bytes, zero only for scrubbed records.
};
// Encoded sizes exclude envelopes and include all string length prefixes.
uint32_t memory_value_size(const MemoryValue &value);
uint32_t memory_record_size(const MemoryRecord &record);
uint32_t memory_record_max(MemoryLimits limits);
// Exact source-reference framing is also used by work and context provenance records.
constexpr uint32_t memory_dependencies_size(uint32_t count) {
    return sizeof(uint32_t) + count * memory_dependency_bytes;
}
void write_memory_dependencies(const MemoryDependencies &dependencies,
                               foundation::BodyWriter &writer);
bool read_memory_dependencies(foundation::BodyReader &reader, MemoryDependencies &out);
// Shared value framing for journal, snapshots and exact command requests.
void write_memory_value(const MemoryValue &value, foundation::BodyWriter &writer);
bool read_memory_value(foundation::BodyReader &reader, MemoryValue &out);
// Reader checks framing only; typed/decoded semantics share check_memory_value/table restore.
void write_memory_record(const MemoryRecord &record, ayni_string_v1 body, uint8_t *out);
bool read_memory_record(const uint8_t *body, uint32_t size, DecodedMemory &out);
} // namespace ayni::knowledge
#endif
