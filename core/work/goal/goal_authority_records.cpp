#include "work/goal/goal_authority_records.h"
#include "foundation/workspace.h"

#include "foundation/record_body.h"

namespace ayni::work {

uint32_t goal_authority_record_size(const GoalAuthorityRecord &record) {
    uint32_t size = goal_authority_record_fixed_size;
    for (uint32_t index = 0; index < record.count; ++index) {
        const auto &requirement = record.requirements[index];
        size += goal_requirement_record_fixed_size + requirement.capability_id.size() +
                requirement.source_domain.size + requirement.scope.size +
                requirement.business_contract.size;
    }
    return size;
}

// One layout serves journal and snapshot; stamps remain explicit so replay
// can distinguish a full snapshot fact from a mismatched transition batch.
void write_goal_authority_record(const GoalAuthorityRecord &record, uint8_t *body) {
    foundation::BodyWriter writer(body, goal_authority_record_size(record));
    writer.u64(record.goal_id);
    writer.u64(record.stamp.transition_id);
    writer.u64(record.stamp.commit_seq);
    writer.u32(static_cast<uint32_t>(record.phase));
    writer.u32(record.count);
    writer.u64(record.binding.established_seq);
    writer.u64(record.binding.parent.goal_id);
    writer.u64(record.binding.parent.commit_seq);
    writer.u64(record.binding.input.incarnation);
    writer.u64(record.binding.input.through);
    for (uint32_t index = 0; index < record.count; ++index) {
        const auto &requirement = record.requirements[index];
        writer.u32(requirement.capability_version);
        writer.bytes(requirement.capability_id.data(), requirement.capability_id.size());
        writer.bytes(requirement.source_domain.data, requirement.source_domain.size);
        writer.bytes(requirement.scope.data, requirement.scope.size);
        writer.u64(requirement.dependency.grant_id);
        writer.u32(requirement.dependency.revision);
        writer.bytes(requirement.business_contract.data, requirement.business_contract.size);
    }
}

namespace {

bool read_requirement(foundation::BodyReader &reader, GoalRequirement &out) {
    out.capability_version = reader.u32();
    uint32_t size = 0;
    const uint8_t *id = reader.bytes(catalog::CatalogId::max_size, size);
    if (!out.capability_id.assign({reinterpret_cast<const char *>(id), size})) {
        return false;
    }
    const uint8_t *domain = reader.bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX, size);
    out.source_domain = {domain, size};
    const uint8_t *scope = reader.bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX, size);
    out.scope = {scope, size};
    out.dependency = {reader.u64(), reader.u32()};
    out.business_contract = reader.bytes(AYNI_BUSINESS_CONTRACT_BYTES_MAX);
    return true;
}

} // namespace

// Decode locally before validation: a malformed later requirement must not
// publish the valid prefix. Struct and byte inputs share every domain rule.
bool read_goal_authority_record(ayni::foundation::Workspace &workspace, const uint8_t *body,
                                uint32_t size, GoalAuthorityRecord &out) {
    if (body == nullptr || size < goal_authority_record_fixed_size) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    GoalAuthorityRecord record{};
    record.goal_id = reader.u64();
    record.stamp = {reader.u64(), reader.u64()};
    record.phase = static_cast<GoalAuthorityPhase>(reader.u32());
    record.count = reader.u32();
    record.binding.established_seq = reader.u64();
    record.binding.parent = {reader.u64(), reader.u64()};
    record.binding.input = {reader.u64(), reader.u64()};
    if (record.count > AYNI_GOAL_REQUIREMENT_CAPACITY_MAX) {
        return false;
    }
    for (uint32_t index = 0; index < record.count; ++index) {
        if (!read_requirement(reader, record.requirements[index])) {
            return false;
        }
    }
    if (!reader.ok() || !valid_goal_authority_record(workspace, record)) {
        return false;
    }
    out = record;
    return true;
}

} // namespace ayni::work
