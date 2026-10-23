#include "work/goal/goal_authority_records.h"

#include "durability/batch/record_body.h"

namespace ayni::work {

uint32_t goal_authority_record_size(const GoalAuthorityRecord &record) {
    uint32_t size = goal_authority_record_fixed_size;
    for (uint32_t index = 0; index < record.count; ++index) {
        const auto &requirement = record.requirements[index];
        size += goal_requirement_record_fixed_size + requirement.capability_id.size() +
                requirement.source_domain.size + requirement.scope.size;
    }
    return size;
}

// One layout serves journal and snapshot; stamps remain explicit so replay
// can distinguish a full snapshot fact from a mismatched transition batch.
void write_goal_authority_record(const GoalAuthorityRecord &record, uint8_t *body) {
    durability::BodyWriter writer(body, goal_authority_record_size(record));
    writer.u64(record.goal_id);
    writer.u64(record.stamp.transition_id);
    writer.u64(record.stamp.commit_seq);
    writer.u32(static_cast<uint32_t>(record.phase));
    writer.u32(record.count);
    for (uint32_t index = 0; index < record.count; ++index) {
        const auto &requirement = record.requirements[index];
        writer.u32(requirement.capability_version);
        writer.bytes(requirement.capability_id.data(), requirement.capability_id.size());
        writer.bytes(requirement.source_domain.data, requirement.source_domain.size);
        writer.bytes(requirement.scope.data, requirement.scope.size);
        writer.u64(requirement.dependency.grant_id);
        writer.u32(requirement.dependency.revision);
    }
}

namespace {

bool read_requirement(durability::BodyReader &reader, GoalRequirement &out) {
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
    return true;
}

} // namespace

// Decode locally before validation: a malformed later requirement must not
// publish the valid prefix. Struct and byte inputs share every domain rule.
bool read_goal_authority_record(const uint8_t *body, uint32_t size, GoalAuthorityRecord &out) {
    if (body == nullptr || size < goal_authority_record_fixed_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    GoalAuthorityRecord record{};
    record.goal_id = reader.u64();
    record.stamp = {reader.u64(), reader.u64()};
    record.phase = static_cast<GoalAuthorityPhase>(reader.u32());
    record.count = reader.u32();
    if (record.count > AYNI_GOAL_REQUIREMENT_CAPACITY_MAX) {
        return false;
    }
    for (uint32_t index = 0; index < record.count; ++index) {
        if (!read_requirement(reader, record.requirements[index])) {
            return false;
        }
    }
    if (!reader.ok() || !valid_goal_authority_record(record)) {
        return false;
    }
    out = record;
    return true;
}

} // namespace ayni::work
