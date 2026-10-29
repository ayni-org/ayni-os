// Goal authority record layout, contracts/storage/records.md type 17/v1.
// Journal and snapshot share this codec; the surrounding replay validates
// owning Goal identity, batch pairing and references to retained Grants.
#ifndef AYNI_CORE_WORK_GOAL_GOAL_AUTHORITY_RECORDS_H
#define AYNI_CORE_WORK_GOAL_GOAL_AUTHORITY_RECORDS_H

#include "durability/batch/record_types.h"
#include "work/goal/goal_authority.h"

#include <cstdint>

namespace ayni::work {

// E updates one Goal ceiling/reference set per transition; F propagation uses
// separate bounded transitions. records.md type 17/v1 has no multi-Goal batch.
constexpr uint32_t goal_authority_records_per_transition = 1;
constexpr uint16_t goal_authority_record_type = durability::record_type::goal_authority;
constexpr uint16_t goal_authority_record_version = 1;
// The original 32-byte identity/stamp/phase/count prefix is followed by five
// u64 binding/frontier words. Each requirement adds version, three byte-string
// lengths, Grant ID and selected revision (records.md type 17/v1).
constexpr uint32_t goal_authority_record_fixed_size = 72;
constexpr uint32_t goal_requirement_record_fixed_size = 28;

// Complete record bounds, using configured requirements and per-region bytes.
constexpr uint32_t goal_authority_record_max(uint32_t requirements, uint32_t scope_bytes) {
    return goal_authority_record_fixed_size +
           requirements * (goal_requirement_record_fixed_size + AYNI_CATALOG_ID_MAX_BYTES +
                           goal_requirement_regions * scope_bytes);
}
// Requires valid_goal_authority_record; no mutation or current-catalog lookup.
uint32_t goal_authority_record_size(const GoalAuthorityRecord &record);
// Requires a validated record and exactly record_size writable bytes that do
// not overlap any borrowed input. Does not repeat semantic admission.
void write_goal_authority_record(const GoalAuthorityRecord &record, uint8_t *body);
// Parses into temporary borrowed views and uses the shared semantic validator.
// Truncation, trailing bytes or illegal facts preserve out entirely.
bool read_goal_authority_record(const uint8_t *body, uint32_t size, GoalAuthorityRecord &out);

} // namespace ayni::work
#endif // AYNI_CORE_WORK_GOAL_GOAL_AUTHORITY_RECORDS_H
