// REQUIRED privacy sweep progress, shared by journal and snapshot (ADR-0060, records.md).
#ifndef AYNI_CORE_WORK_PRIVACY_RECORDS_H
#define AYNI_CORE_WORK_PRIVACY_RECORDS_H
#include "durability/batch/record_types.h"
#include "work/privacy/privacy_cleanup.h"
namespace ayni::work {
constexpr uint16_t privacy_record_type = durability::record_type::privacy_cleanup;
constexpr uint16_t privacy_record_version = 1;
constexpr uint32_t privacy_record_size = 48; // request, phase/effect, cursor, generation, stamp.
// Body has exactly privacy_record_size bytes; no native struct padding is persisted.
void write_privacy_record(const PrivacyRecord &record, uint8_t *body);
// Shape and shared intrinsic state validation; preserves output on refusal.
bool read_privacy_record(const uint8_t *body, uint32_t size, PrivacyRecord &out);
} // namespace ayni::work
#endif
