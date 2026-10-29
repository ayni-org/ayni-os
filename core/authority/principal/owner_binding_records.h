// The single OWNER_BINDING record: which authenticated owner first bound
// this store. A Host's current expectation never overwrites this fact.
// Layout: contracts/storage/records.md, type 12/v1 (ADR-0028).

#ifndef AYNI_CORE_AUTHORITY_PRINCIPAL_OWNER_BINDING_RECORDS_H
#define AYNI_CORE_AUTHORITY_PRINCIPAL_OWNER_BINDING_RECORDS_H

#include "durability/batch/record_types.h"

#include <cstdint>

namespace ayni::authority {

constexpr uint16_t owner_binding_record_type = durability::record_type::owner_binding;
constexpr uint16_t owner_binding_record_version = 1;
constexpr uint16_t owner_binding_snapshot_record_version = 2; // Adds original binding stamp.
constexpr uint32_t owner_binding_record_size = 16;

struct OwnerBinding;

struct OwnerBindingRecord {
    uint64_t principal_id;
    uint32_t binding_revision;
};

// body has owner_binding_record_size bytes. The stamp belongs to the batch;
// snapshots append the binding's original stamp after these same fields.
void write_owner_binding_record(const OwnerBinding &binding, uint8_t *body);
// Rejects other sizes, zero identities/revisions and nonzero reserved bytes.
// Leaves out unchanged on failure.
bool read_owner_binding_record(const uint8_t *body, uint32_t size, OwnerBindingRecord &out);

} // namespace ayni::authority

#endif // AYNI_CORE_AUTHORITY_PRINCIPAL_OWNER_BINDING_RECORDS_H
