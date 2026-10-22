// ADR-0030/0031: Grant facts contain durations, never a boot clock or gates.
#ifndef AYNI_CORE_AUTHORITY_GRANT_GRANT_RECORDS_H
#define AYNI_CORE_AUTHORITY_GRANT_GRANT_RECORDS_H

#include "authority/grant/grant_table.h"

#include <cstdint>

namespace ayni::authority {

constexpr uint16_t grant_record_type = 13;
constexpr uint16_t grant_record_version = 1;
constexpr uint16_t grant_state_record_type = 14;
constexpr uint16_t grant_state_record_version = 1;
constexpr uint16_t grant_revoked_state_record_version = 2;
constexpr uint16_t grant_revocation_record_type = 15;
constexpr uint16_t grant_revocation_record_version = 1;
constexpr uint16_t grant_revocation_progress_record_type = 16;
constexpr uint16_t grant_revocation_progress_record_version = 1;
constexpr uint16_t grant_snapshot_record_version = 2;
constexpr uint16_t grant_snapshot_legacy_record_version = 1;
constexpr uint32_t grant_record_fixed_size = 80;
constexpr uint32_t grant_state_record_size = 16;
constexpr uint32_t grant_revoked_state_record_size = 24;
constexpr uint32_t grant_revocation_record_size = 32;
constexpr uint32_t grant_revocation_progress_record_size = 24;
constexpr uint32_t grant_snapshot_tail_size = 40;
constexpr uint32_t grant_revocation_snapshot_tail_size = 80;

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
    uint64_t revoked_by_root_id = 0;
};

struct GrantSnapshotRecord {
    GrantRecord create;
    foundation::TransitionStamp created_stamp;
    foundation::TransitionStamp stamp;
    uint32_t revision;
    ayni_grant_lifecycle_t lifecycle;
    uint64_t revoked_by_root_id = 0;
    uint64_t cleanup_after_grant_id = 0;
    uint64_t cleanup_through_grant_id = 0;
    foundation::TransitionStamp cleanup_stamp{};
};

struct GrantRevocationRecord {
    uint64_t grant_id;
    uint32_t revision;
    ayni_intent_effect_t effect;
    uint64_t cleanup_after_grant_id;
    uint64_t cleanup_through_grant_id;
};

struct GrantRevocationProgressRecord {
    uint64_t root_grant_id;
    uint64_t after_grant_id;
    uint64_t through_grant_id;
};

// Representation-independent record facts; current catalog is not consulted.
bool valid_grant_record(const GrantRecord &record);

constexpr uint32_t grant_record_max(uint32_t scope_max_bytes) {
    return grant_record_fixed_size + AYNI_CATALOG_ID_MAX_BYTES + 2U * scope_max_bytes;
}
constexpr uint32_t grant_snapshot_record_max(uint32_t scope_max_bytes) {
    return grant_record_max(scope_max_bytes) + grant_revocation_snapshot_tail_size;
}
uint32_t grant_record_size(const Grant &grant);
void write_grant_record(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                        uint8_t *body);
bool read_grant_record(const uint8_t *body, uint32_t size, GrantRecord &out);
void write_grant_state_record(const Grant &grant, uint8_t *body);
uint16_t grant_state_record_version_for(const Grant &grant);
uint32_t grant_state_record_size_for(const Grant &grant);
bool read_grant_state_record(const uint8_t *body, uint32_t size, GrantStateRecord &out,
                             uint16_t version = grant_state_record_version);
uint32_t grant_snapshot_record_size(const Grant &grant,
                                    uint16_t version = grant_snapshot_legacy_record_version);
// Unsupported versions and v1 REVOKED have size zero; their writer is a no-op.
void write_grant_snapshot_record(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                                 uint8_t *body,
                                 uint16_t version = grant_snapshot_legacy_record_version);
bool read_grant_snapshot_record(const uint8_t *body, uint32_t size, GrantSnapshotRecord &out,
                                uint16_t version = grant_snapshot_legacy_record_version);
void write_grant_revocation_record(const GrantTable::RevokePlan &plan, uint8_t *body);
bool read_grant_revocation_record(const uint8_t *body, uint32_t size, GrantRevocationRecord &out);
void write_grant_revocation_progress_record(const GrantTable::CleanupPlan &plan, uint8_t *body);
bool read_grant_revocation_progress_record(const uint8_t *body, uint32_t size,
                                           GrantRevocationProgressRecord &out);

} // namespace ayni::authority
#endif // AYNI_CORE_AUTHORITY_GRANT_GRANT_RECORDS_H
