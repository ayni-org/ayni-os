// REQUIRED type26/v1 content anchors; one shared layout for journal and snapshots (ADR-0060).
#ifndef AYNI_CORE_KNOWLEDGE_CONTEXT_RECORDS_H
#define AYNI_CORE_KNOWLEDGE_CONTEXT_RECORDS_H
#include "knowledge/context/context_table.h"
#include "knowledge/memory/memory_records.h"
namespace ayni::knowledge {
constexpr uint16_t context_record_type = durability::record_type::context;
constexpr uint16_t context_record_version = 1;
constexpr uint32_t context_record_fixed_bytes = 80; // Identity, revision, file action and stamps.
constexpr uint32_t context_record_size(uint32_t dependencies) {
    return context_record_fixed_bytes + memory_dependencies_size(dependencies);
}
void write_context_record(const ContextRecord &record, uint8_t *body); // Exactly size(count) bytes.
bool read_context_record(const uint8_t *body, uint32_t size,
                         ContextRecord &out); // Atomic validated output.
} // namespace ayni::knowledge
#endif
