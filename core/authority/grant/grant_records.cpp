#include "authority/grant/grant_records.h"

#include "authority/scope/scope.h"
#include "durability/batch/record_body.h"

namespace ayni::authority {
namespace {

constexpr uint32_t stamp_size = 16;
constexpr uint32_t created_stamp_at = 0;
constexpr uint32_t last_stamp_at = created_stamp_at + stamp_size;
constexpr uint32_t revision_at = last_stamp_at + stamp_size;
constexpr uint32_t lifecycle_at = revision_at + sizeof(uint32_t);

bool kernel_id(uint64_t id) {
    return (id >> 32U) != 0U && static_cast<uint32_t>(id) != 0U;
}

void write_stamp(foundation::TransitionStamp stamp, uint8_t *body) {
    foundation::store_le64(body, stamp.transition_id);
    foundation::store_le64(body + sizeof(uint64_t), stamp.commit_seq);
}

foundation::TransitionStamp read_stamp(const uint8_t *body) {
    return {foundation::load_le64(body), foundation::load_le64(body + sizeof(uint64_t))};
}

} // namespace

bool valid_grant_record(const GrantRecord &record) {
    const NewGrant &grant = record.grant;
    const bool permanent = grant.lifetime == AYNI_GRANT_PERMANENT;
    const bool limited = grant.lifetime == AYNI_GRANT_CURRENT_BOOT;
    if (!kernel_id(record.grant_id) || grant.subject_instance_id == 0U ||
        grant.owner_principal_id == 0U || grant.owner_binding_revision == 0U ||
        grant.capability_id.size() == 0U || grant.capability_version == 0U ||
        (grant.flags & ~AYNI_GRANT_ALLOW_DELEGATION) != 0U || (!permanent && !limited) ||
        (permanent ? grant.duration_us != 0U : grant.duration_us == 0U) ||
        record.depth > AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX ||
        (grant.parent_grant_id == 0U ? record.depth != 0U
                                     : !kernel_id(grant.parent_grant_id) || record.depth == 0U) ||
        (record.replaced_grant_id != 0U &&
         (!kernel_id(record.replaced_grant_id) || record.replaced_grant_id == record.grant_id)) ||
        grant.source_domain.size > AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX ||
        record.scope.size > AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX) {
        return false;
    }
    const auto covered = covers(grant.source_domain, grant.source_domain, record.scope);
    return covered.status == AYNI_OK && covered.covered;
}

uint32_t grant_record_size(const Grant &grant) {
    return grant_record_fixed_size + grant.capability_id.size() + grant.source_domain_size +
           grant.scope_size;
}

void write_grant_record(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                        uint8_t *body) {
    durability::BodyWriter writer(body, grant_record_size(grant));
    writer.u64(grant.grant_id);
    writer.u64(grant.replaced_grant_id);
    writer.u64(grant.subject_instance_id);
    writer.u64(grant.parent_grant_id);
    writer.u64(grant.owner_principal_id);
    writer.u32(grant.owner_binding_revision);
    writer.u32(grant.capability_version);
    writer.bytes(grant.capability_id.data(), grant.capability_id.size());
    writer.u32(grant.lifetime);
    writer.u32(grant.allow_delegation ? 1U : 0U);
    writer.u64(grant.duration_us);
    writer.u32(grant.depth);
    writer.bytes(domain.data, domain.size);
    writer.bytes(scope.data, scope.size);
}

bool read_grant_record(const uint8_t *body, uint32_t size, GrantRecord &out) {
    if (body == nullptr || size < grant_record_fixed_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    GrantRecord record{};
    record.grant_id = reader.u64();
    record.replaced_grant_id = reader.u64();
    record.grant.subject_instance_id = reader.u64();
    record.grant.parent_grant_id = reader.u64();
    record.grant.owner_principal_id = reader.u64();
    record.grant.owner_binding_revision = reader.u32();
    record.grant.capability_version = reader.u32();
    uint32_t id_size = 0;
    const uint8_t *id = reader.bytes(catalog::CatalogId::max_size, id_size);
    if (!record.grant.capability_id.assign({reinterpret_cast<const char *>(id), id_size})) {
        return false;
    }
    record.grant.lifetime = reader.u32();
    const uint32_t delegation = reader.u32();
    record.grant.flags = delegation == 1U ? AYNI_GRANT_ALLOW_DELEGATION : 0U;
    record.grant.duration_us = reader.u64();
    record.depth = reader.u32();
    uint32_t domain_size = 0;
    const uint8_t *domain = reader.bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX, domain_size);
    record.grant.source_domain = {domain, domain_size};
    uint32_t scope_size = 0;
    const uint8_t *scope = reader.bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX, scope_size);
    record.scope = {scope, scope_size};
    if (!reader.ok() || delegation > 1U || !valid_grant_record(record)) {
        return false;
    }
    out = record;
    return true;
}

void write_grant_state_record(const Grant &grant, uint8_t *body) {
    durability::BodyWriter writer(body, grant_state_record_size);
    writer.u64(grant.grant_id);
    writer.u32(grant.revision);
    writer.u32(grant.lifecycle);
}

bool read_grant_state_record(const uint8_t *body, uint32_t size, GrantStateRecord &out) {
    if (body == nullptr || size != grant_state_record_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    const GrantStateRecord record{reader.u64(), reader.u32(), reader.u32()};
    if (!reader.ok() || !kernel_id(record.grant_id) || record.revision != 2U ||
        record.lifecycle != AYNI_GRANT_EXPIRED) {
        return false;
    }
    out = record;
    return true;
}

uint32_t grant_snapshot_record_size(const Grant &grant) {
    return grant_record_size(grant) + grant_snapshot_tail_size;
}

void write_grant_snapshot_record(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                                 uint8_t *body) {
    write_grant_record(grant, domain, scope, body);
    uint8_t *tail = body + grant_record_size(grant);
    write_stamp(grant.created_stamp, tail + created_stamp_at);
    write_stamp(grant.stamp, tail + last_stamp_at);
    foundation::store_le32(tail + revision_at, grant.revision);
    foundation::store_le32(tail + lifecycle_at, grant.lifecycle);
}

bool read_grant_snapshot_record(const uint8_t *body, uint32_t size, GrantSnapshotRecord &out) {
    if (body == nullptr || size < grant_record_fixed_size + grant_snapshot_tail_size) {
        return false;
    }
    const uint32_t create_size = size - grant_snapshot_tail_size;
    GrantSnapshotRecord record{};
    if (!read_grant_record(body, create_size, record.create)) {
        return false;
    }
    const uint8_t *tail = body + create_size;
    record.created_stamp = read_stamp(tail + created_stamp_at);
    record.stamp = read_stamp(tail + last_stamp_at);
    record.revision = foundation::load_le32(tail + revision_at);
    record.lifecycle = foundation::load_le32(tail + lifecycle_at);
    out = record;
    return true;
}

} // namespace ayni::authority
