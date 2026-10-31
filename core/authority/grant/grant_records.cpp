#include "authority/grant/grant_records.h"
#include "foundation/workspace.h"

#include "authority/scope/scope.h"
#include "catalog/capability/business_encoding.h"
#include "foundation/kernel_id.h"
#include "foundation/record_body.h"

namespace ayni::authority {
namespace {

constexpr uint32_t stamp_size = 16;
constexpr uint32_t created_stamp_at = 0;
constexpr uint32_t last_stamp_at = created_stamp_at + stamp_size;
constexpr uint32_t revision_at = last_stamp_at + stamp_size;
constexpr uint32_t lifecycle_at = revision_at + sizeof(uint32_t);
// records.md: schema 4 snapshot v2 always includes the complete cleanup tail.
constexpr uint32_t revoker_at = lifecycle_at + sizeof(uint32_t);
constexpr uint32_t cleanup_after_at = revoker_at + sizeof(uint64_t);
constexpr uint32_t cleanup_through_at = cleanup_after_at + sizeof(uint64_t);
constexpr uint32_t cleanup_stamp_at = cleanup_through_at + sizeof(uint64_t);

void write_stamp(foundation::TransitionStamp stamp, uint8_t *body) {
    foundation::store_le64(body, stamp.transition_id);
    foundation::store_le64(body + sizeof(uint64_t), stamp.commit_seq);
}

foundation::TransitionStamp read_stamp(const uint8_t *body) {
    return {foundation::load_le64(body), foundation::load_le64(body + sizeof(uint64_t))};
}

} // namespace

bool read_grant_record_id(const uint8_t *body, uint32_t size, uint64_t &out_id) {
    if (body == nullptr || size < sizeof(uint64_t)) {
        return false;
    }
    out_id = foundation::load_le64(body);
    return true;
}

// Byte and struct readers share the accepted-fact rules. Deferred request
// flags cannot appear in storage, and the saved domain must cover the full scope.
bool valid_grant_record(ayni::foundation::Workspace &workspace, const GrantRecord &record) {
    const NewGrant &grant = record.grant;
    if (!foundation::is_kernel_id(record.grant_id) || grant.subject_instance_id == 0U ||
        grant.owner_principal_id == 0U || grant.owner_binding_revision == 0U ||
        grant.capability_id.size() == 0U || grant.capability_version == 0U ||
        !valid_persisted_grant_flags(grant.flags) ||
        !valid_goal_binding(grant.bound_goal_id, grant.parent_grant_id, grant.flags) ||
        (grant.bound_goal_id != 0U && grant.bound_goal_id >= record.grant_id) ||
        !valid_grant_lifetime(grant.lifetime, grant.duration_us) ||
        record.depth > AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX ||
        (grant.parent_grant_id == 0U
             ? record.depth != 0U
             : !foundation::is_kernel_id(grant.parent_grant_id) || record.depth == 0U) ||
        (record.replaced_grant_id != 0U && (!foundation::is_kernel_id(record.replaced_grant_id) ||
                                            record.replaced_grant_id == record.grant_id)) ||
        grant.source_domain.size > AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX ||
        record.scope.size > AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX ||
        grant.business_contract.size > AYNI_BUSINESS_CONTRACT_BYTES_MAX ||
        (grant.business_contract.size != 0U && grant.business_contract.data == nullptr)) {
        return false;
    }
    const auto covered = covers(grant.source_domain, grant.source_domain, record.scope);
    if (covered.status != AYNI_OK || !covered.covered) {
        return false;
    }
    if (grant.business_contract.size != 0U) {
        auto proof_lease = workspace.make<catalog::DecodedBusiness>();
        if (!proof_lease) {
            return false;
        }
        auto &proof = *proof_lease;
        return catalog::decode_business(workspace, grant.source_domain, grant.business_contract,
                                        proof) == AYNI_OK;
    }
    return true;
}

uint32_t grant_record_size(const Grant &grant) {
    return grant_record_fixed_size + grant.capability_id.size() + grant.source_domain_size +
           grant.scope_size + grant.business_contract_size;
}

void write_grant_record(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                        ayni_bytes_v1 business, uint8_t *body) {
    foundation::BodyWriter writer(body, grant_record_size(grant));
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
    writer.u64(grant.bound_goal_id);
    writer.bytes(business.data, business.size);
}

// Current creation has one complete body for ordinary and Goal-bound facts.
bool read_grant_record(ayni::foundation::Workspace &workspace, const uint8_t *body, uint32_t size,
                       GrantRecord &out) {
    if (body == nullptr || size < grant_record_fixed_size) {
        return false;
    }
    foundation::BodyReader reader(body, size);
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
    record.grant.bound_goal_id = reader.u64();
    record.grant.business_contract = reader.bytes(AYNI_BUSINESS_CONTRACT_BYTES_MAX);
    if (!reader.ok() || delegation > 1U || !valid_grant_record(workspace, record)) {
        return false;
    }
    out = record;
    return true;
}

void write_grant_state_record(const Grant &grant, uint8_t *body) {
    foundation::BodyWriter writer(body, grant_state_record_size);
    writer.u64(grant.grant_id);
    writer.u32(grant.revision);
    writer.u32(grant.lifecycle);
    writer.u64(grant.revoked_by_root_id);
}

// Lifecycle determines the proof: revoked children name their root, while expiry
// and Goal termination require work cleanup. No historical version carries that rule.
bool read_grant_state_record(const uint8_t *body, uint32_t size, GrantStateRecord &out) {
    if (body == nullptr || size != grant_state_record_size) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    GrantStateRecord record{reader.u64(), reader.u32(), reader.u32(), reader.u64()};
    const bool revoked = record.lifecycle == AYNI_GRANT_REVOKED;
    if (!reader.ok() || !foundation::is_kernel_id(record.grant_id) ||
        record.revision != grant_terminal_revision ||
        (revoked ? !foundation::is_kernel_id(record.revoked_by_root_id) ||
                       is_revocation_root(record.grant_id, record.revoked_by_root_id)
                 : record.revoked_by_root_id != 0 || (record.lifecycle != AYNI_GRANT_GOAL_ENDED &&
                                                      record.lifecycle != AYNI_GRANT_EXPIRED))) {
        return false;
    }
    out = record;
    return true;
}

uint32_t grant_snapshot_record_size(const Grant &grant) {
    return grant_record_size(grant) + grant_snapshot_tail_size;
}

void write_grant_snapshot_record(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                                 ayni_bytes_v1 business, uint8_t *body) {
    write_grant_record(grant, domain, scope, business, body);
    uint8_t *tail = body + grant_record_size(grant);
    write_stamp(grant.created_stamp, tail + created_stamp_at);
    write_stamp(grant.stamp, tail + last_stamp_at);
    foundation::store_le32(tail + revision_at, grant.revision);
    foundation::store_le32(tail + lifecycle_at, grant.lifecycle);
    foundation::store_le64(tail + revoker_at, grant.revoked_by_root_id);
    foundation::store_le64(tail + cleanup_after_at, grant.cleanup_after_grant_id);
    foundation::store_le64(tail + cleanup_through_at, grant.cleanup_through_grant_id);
    write_stamp(grant.cleanup_stamp, tail + cleanup_stamp_at);
}

// The complete snapshot tail always preserves original stamps and cleanup facts.
bool read_grant_snapshot_record(ayni::foundation::Workspace &workspace, const uint8_t *body,
                                uint32_t size, GrantSnapshotRecord &out) {
    if (body == nullptr || size < grant_record_fixed_size + grant_snapshot_tail_size) {
        return false;
    }
    const uint32_t create_size = size - grant_snapshot_tail_size;
    GrantSnapshotRecord record{};
    if (!read_grant_record(workspace, body, create_size, record.create)) {
        return false;
    }
    const uint8_t *tail = body + create_size;
    record.created_stamp = read_stamp(tail + created_stamp_at);
    record.stamp = read_stamp(tail + last_stamp_at);
    record.revision = foundation::load_le32(tail + revision_at);
    record.lifecycle = foundation::load_le32(tail + lifecycle_at);
    record.revoked_by_root_id = foundation::load_le64(tail + revoker_at);
    record.cleanup_after_grant_id = foundation::load_le64(tail + cleanup_after_at);
    record.cleanup_through_grant_id = foundation::load_le64(tail + cleanup_through_at);
    record.cleanup_stamp = read_stamp(tail + cleanup_stamp_at);
    out = record;
    return true;
}

void write_grant_revocation_record(const GrantTable::RevokePlan &plan, uint8_t *body) {
    foundation::BodyWriter writer(body, grant_revocation_record_size);
    writer.u64(plan.grant_id);
    writer.u32(plan.revision);
    writer.u32(plan.effect);
    writer.u64(plan.cleanup_after_grant_id);
    writer.u64(plan.cleanup_through_grant_id);
}

// C-AUTH-027 stores only a result possible for accepted root control. Decode
// fixed offsets here; live/replay relationship checks remain with GrantTable.
bool read_grant_revocation_record(const uint8_t *body, uint32_t size, GrantRevocationRecord &out) {
    if (body == nullptr || size != grant_revocation_record_size) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    const GrantRevocationRecord record{reader.u64(), reader.u32(), reader.u32(), reader.u64(),
                                       reader.u64()};
    if (!reader.ok() || !foundation::is_kernel_id(record.grant_id) ||
        record.cleanup_after_grant_id != 0U) {
        return false;
    }
    if (record.effect == AYNI_INTENT_EFFECT_CHANGED) {
        if (record.revision != grant_terminal_revision ||
            !foundation::is_kernel_id(record.cleanup_through_grant_id) ||
            record.cleanup_through_grant_id < record.grant_id) {
            return false;
        }
    } else if (record.cleanup_through_grant_id != 0U ||
               (record.effect == AYNI_INTENT_EFFECT_NO_CHANGE
                    ? record.revision != grant_terminal_revision
                    : record.effect != AYNI_INTENT_EFFECT_TARGET_GONE || record.revision != 0U)) {
        return false;
    }
    out = record;
    return true;
}

void write_grant_revocation_progress_record(const GrantTable::CleanupPlan &plan, uint8_t *body) {
    foundation::BodyWriter writer(body, grant_revocation_progress_record_size);
    writer.u64(plan.root_grant_id);
    writer.u64(plan.after_grant_id);
    writer.u64(plan.through_grant_id);
}

// C-AUTH-026 progress carries IDs and stamps, never a physical slot cursor.
// Shape checks precede GrantTable ordering and same-batch relationship checks.
bool read_grant_revocation_progress_record(const uint8_t *body, uint32_t size,
                                           GrantRevocationProgressRecord &out) {
    if (body == nullptr || size != grant_revocation_progress_record_size) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    const GrantRevocationProgressRecord record{reader.u64(), reader.u64(), reader.u64()};
    if (!reader.ok() || !foundation::is_kernel_id(record.root_grant_id) ||
        !foundation::is_kernel_id(record.after_grant_id) ||
        !foundation::is_kernel_id(record.through_grant_id) ||
        record.after_grant_id > record.through_grant_id ||
        record.through_grant_id < record.root_grant_id) {
        return false;
    }
    out = record;
    return true;
}

} // namespace ayni::authority
