// ADR-0030: field-coded Grant facts contain durations, never a boot clock.
#ifndef AYNI_CORE_AUTHORITY_GRANT_GRANT_RECORDS_H
#define AYNI_CORE_AUTHORITY_GRANT_GRANT_RECORDS_H

#include "authority/grant/grant_table.h"

#include <cstdint>

namespace ayni::authority {

constexpr uint16_t grant_record_type = 13;
constexpr uint16_t grant_record_version = 1;
constexpr uint16_t grant_state_record_type = 14;
constexpr uint16_t grant_state_record_version = 1;
constexpr uint32_t grant_record_fixed_size = 80;
constexpr uint32_t grant_state_record_size = 16;
constexpr uint32_t grant_snapshot_tail_size = 40;

struct GrantRecord {
    uint64_t grant_id;
    uint64_t replaced_grant_id;
    uint32_t depth;
    NewGrant grant; // scope is null: record.scope is already canonical
    ayni_bytes_v1 scope;
};

struct GrantStateRecord {
    uint64_t grant_id;
    uint32_t revision;
    ayni_grant_lifecycle_t lifecycle;
};

struct GrantSnapshotRecord {
    GrantRecord create;
    foundation::TransitionStamp created_stamp;
    foundation::TransitionStamp stamp;
    uint32_t revision;
    ayni_grant_lifecycle_t lifecycle;
};

// Representation-independent record facts; current catalog is not consulted.
bool valid_grant_record(const GrantRecord &record);

constexpr uint32_t grant_record_max(uint32_t scope_max_bytes) {
    return grant_record_fixed_size + AYNI_CATALOG_ID_MAX_BYTES + 2U * scope_max_bytes;
}
constexpr uint32_t grant_snapshot_record_max(uint32_t scope_max_bytes) {
    return grant_record_max(scope_max_bytes) + grant_snapshot_tail_size;
}
uint32_t grant_record_size(const Grant &grant);
void write_grant_record(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                        uint8_t *body);
bool read_grant_record(const uint8_t *body, uint32_t size, GrantRecord &out);
void write_grant_state_record(const Grant &grant, uint8_t *body);
bool read_grant_state_record(const uint8_t *body, uint32_t size, GrantStateRecord &out);
uint32_t grant_snapshot_record_size(const Grant &grant);
void write_grant_snapshot_record(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                                 uint8_t *body);
bool read_grant_snapshot_record(const uint8_t *body, uint32_t size, GrantSnapshotRecord &out);

} // namespace ayni::authority
#endif // AYNI_CORE_AUTHORITY_GRANT_GRANT_RECORDS_H
