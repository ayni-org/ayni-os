// REQUIRED type24/v1, work content provenance; contracts/storage/records.md.
#ifndef AYNI_CORE_WORK_PROVENANCE_RECORDS_H
#define AYNI_CORE_WORK_PROVENANCE_RECORDS_H
#include "knowledge/memory/memory_records.h"
#include "work/provenance/provenance_table.h"
namespace ayni::work {
constexpr uint16_t provenance_record_type = durability::record_type::provenance;
constexpr uint16_t provenance_record_version = 1;
// Work ID/kind/content state and transition/commit stamps, followed by exact dependencies.
constexpr uint32_t provenance_record_header_bytes = 32;
constexpr uint32_t provenance_record_size(uint32_t dependencies) {
    return provenance_record_header_bytes + knowledge::memory_dependencies_size(dependencies);
}
// Exact row body, with no runtime slot index or native struct representation.
void write_provenance_record(const ProvenanceRecord &record, uint8_t *body);
bool read_provenance_record(const uint8_t *body, uint32_t size, ProvenanceRecord &out);
} // namespace ayni::work
#endif
