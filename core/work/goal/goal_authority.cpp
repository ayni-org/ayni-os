#include "work/goal/goal_authority.h"

#include "authority/scope/scope.h"
#include "foundation/kernel_id.h"

#include <cstring>
#include <new>

namespace ayni::work {
namespace {

// Arena construction and placement share these validated dimensions; neither
// a byte offset nor the constructor's element loop may invent another stride.
uint32_t requirement_count(GoalAuthorityTable::Limits limits) {
    return limits.goals * limits.requirements;
}

std::size_t requirement_bytes(uint32_t scope_bytes) {
    return static_cast<std::size_t>(goal_requirement_regions) * scope_bytes;
}

bool valid_requirement(const GoalRequirement &requirement) {
    if (requirement.capability_id.size() == 0U || requirement.capability_version == 0U ||
        requirement.source_domain.size > AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX ||
        requirement.scope.size > AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX ||
        !valid_authority_reference(requirement.dependency)) {
        return false;
    }
    const auto coverage =
        authority::covers(requirement.source_domain, requirement.source_domain, requirement.scope);
    return coverage.status == AYNI_OK && coverage.covered;
}

} // namespace

// The stored phase determines completeness, not current Grant validity.
// Proposed needs at least one missing dependency; both other phases retain
// complete selections, even after revocation makes those selections unusable.
bool valid_goal_authority_record(const GoalAuthorityRecord &record) {
    if (!foundation::is_kernel_id(record.goal_id) || record.stamp.transition_id == 0U ||
        record.stamp.commit_seq == 0U || record.count > AYNI_GOAL_REQUIREMENT_CAPACITY_MAX ||
        (record.phase != GoalAuthorityPhase::proposed &&
         record.phase != GoalAuthorityPhase::authorized &&
         record.phase != GoalAuthorityPhase::paused)) {
        return false;
    }
    bool missing = false;
    for (uint32_t index = 0; index < record.count; ++index) {
        if (!valid_requirement(record.requirements[index])) {
            return false;
        }
        missing = missing || record.requirements[index].dependency.grant_id == 0U;
    }
    return record.phase == GoalAuthorityPhase::proposed ? missing : !missing;
}

std::size_t GoalAuthorityTable::storage_size(Limits limits) {
    const std::size_t count = requirement_count(limits);
    return sizeof(Slot) * limits.goals + sizeof(Requirement) * count +
           (count + limits.requirements) * requirement_bytes(limits.scope_bytes);
}

// Both metadata structs contain uint64_t-aligned members; their array sizes
// preserve that alignment. Byte regions have no stricter alignment requirement.
GoalAuthorityTable::GoalAuthorityTable(void *storage, Limits limits)
    : slots_(static_cast<Slot *>(storage)),
      requirements_(reinterpret_cast<Requirement *>(slots_ + limits.goals)),
      bytes_(reinterpret_cast<uint8_t *>(requirements_ + requirement_count(limits))),
      limits_(limits) {
    for (uint32_t index = 0; index < limits.goals; ++index) {
        new (slots_ + index) Slot{};
    }
    const uint32_t count = requirement_count(limits);
    for (uint32_t index = 0; index < count; ++index) {
        new (requirements_ + index) Requirement{};
    }
}

uint32_t GoalAuthorityTable::requirement_index(uint32_t goal_index, uint32_t requirement) const {
    return goal_index * limits_.requirements + requirement;
}

uint8_t *GoalAuthorityTable::domain_at(uint32_t index) const {
    return bytes_ + static_cast<std::size_t>(index) * requirement_bytes(limits_.scope_bytes);
}

uint8_t *GoalAuthorityTable::scope_at(uint32_t index) const {
    return domain_at(index) + limits_.scope_bytes;
}

// The caller already validated accepted facts once (ADR-0037 decision 3).
// Finish configured capacity checks before copying borrowed immutable views;
// no callback or Grant lookup may intervene before publishing the complete slot.
ayni_status_t GoalAuthorityTable::install_validated(uint32_t goal_index,
                                                    const GoalAuthorityRecord &record) {
    if (goal_index >= limits_.goals || record.count > limits_.requirements) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    for (uint32_t index = 0; index < record.count; ++index) {
        const auto &input = record.requirements[index];
        if (input.source_domain.size > limits_.scope_bytes ||
            input.scope.size > limits_.scope_bytes) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    }
    for (uint32_t index = 0; index < record.count; ++index) {
        const auto &input = record.requirements[index];
        const uint32_t at = requirement_index(goal_index, index);
        requirements_[at] = {input.capability_id, input.capability_version,
                             input.source_domain.size, input.scope.size, input.dependency};
        std::memcpy(domain_at(at), input.source_domain.data, input.source_domain.size);
        std::memcpy(scope_at(at), input.scope.data, input.scope.size);
    }
    slots_[goal_index] = {record.goal_id, record.stamp, record.phase, record.count, true};
    return AYNI_OK;
}

// Return all views together, so neither an absent row nor a reused Goal slot
// can accidentally publish part of another Goal's saved authority.
bool GoalAuthorityTable::read(uint32_t goal_index, uint64_t goal_id,
                              GoalAuthorityRecord &out) const {
    if (!contains(goal_index, goal_id)) {
        return false;
    }
    out = at(goal_index);
    return true;
}

// All metadata belongs to this slot; no current Grant/catalog interpretation.
GoalAuthorityRecord GoalAuthorityTable::at(uint32_t goal_index) const {
    const Slot &slot = slots_[goal_index];
    GoalAuthorityRecord record{};
    record.goal_id = slot.goal_id;
    record.stamp = slot.stamp;
    record.phase = slot.phase;
    record.count = slot.count;
    for (uint32_t index = 0; index < slot.count; ++index) {
        const uint32_t at = requirement_index(goal_index, index);
        const Requirement &saved = requirements_[at];
        record.requirements[index] = {saved.capability_id,
                                      saved.capability_version,
                                      {domain_at(at), saved.domain_size},
                                      {scope_at(at), saved.scope_size},
                                      saved.dependency};
    }
    return record;
}

bool GoalAuthorityTable::contains(uint32_t goal_index, uint64_t goal_id) const {
    return goal_index < limits_.goals && slots_[goal_index].present &&
           slots_[goal_index].goal_id == goal_id;
}

// Restore changes references and phase, never the original Goal ceiling.
// Compare sizes before bytes; all views were validated by the record reader.
bool GoalAuthorityTable::same_ceiling(uint32_t goal_index,
                                      const GoalAuthorityRecord &record) const {
    if (slots_[goal_index].count != record.count) {
        return false;
    }
    for (uint32_t index = 0; index < record.count; ++index) {
        const uint32_t at = requirement_index(goal_index, index);
        const auto &saved = requirements_[at];
        const auto &next = record.requirements[index];
        if (!saved.capability_id.equals({next.capability_id.data(), next.capability_id.size()}) ||
            saved.capability_version != next.capability_version ||
            saved.domain_size != next.source_domain.size || saved.scope_size != next.scope.size ||
            std::memcmp(domain_at(at), next.source_domain.data, saved.domain_size) != 0 ||
            std::memcmp(scope_at(at), next.scope.data, saved.scope_size) != 0) {
            return false;
        }
    }
    return true;
}

ayni_status_t GoalAuthorityTable::restore_validated(uint32_t goal_index,
                                                    const GoalAuthorityRecord &record) {
    if (goal_index >= limits_.goals) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const Slot &previous = slots_[goal_index];
    if (previous.present &&
        (previous.goal_id != record.goal_id || !same_ceiling(goal_index, record) ||
         record.stamp.commit_seq <= previous.stamp.commit_seq ||
         record.stamp.transition_id <= previous.stamp.transition_id ||
         (previous.phase != GoalAuthorityPhase::proposed &&
          record.phase == GoalAuthorityPhase::proposed))) {
        return AYNI_INVALID_ARGUMENT;
    }
    return install_validated(goal_index, record);
}

// Normalize once while the exporter still owns its temporary typed views.
// The extra byte row is disjoint from every live slot (ADR-0037).
ayni_status_t GoalAuthorityTable::stage(GoalAuthorityRecord &out,
                                        const catalog::CatalogId &capability, uint32_t version,
                                        ayni_bytes_v1 domain, const ayni_scope_v1 &scope) {
    if (out.count >= limits_.requirements || domain.size > limits_.scope_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const uint32_t index = requirement_count(limits_) + out.count;
    uint32_t size = 0;
    const auto status =
        authority::normalize_scope(domain, scope, scope_at(index), limits_.scope_bytes, size);
    if (status != AYNI_OK) {
        return status == AYNI_BUFFER_TOO_SMALL ? AYNI_CAPACITY_EXCEEDED : status;
    }
    std::memcpy(domain_at(index), domain.data, domain.size);
    out.requirements[out.count++] = {
        capability, version, {domain_at(index), domain.size}, {scope_at(index), size}, {}};
    return AYNI_OK;
}

uint32_t missing_requirement_mask(const GoalAuthorityRecord &record) {
    uint32_t mask = 0;
    for (uint32_t index = 0; index < record.count; ++index) {
        if (record.requirements[index].dependency.grant_id == 0U) {
            mask |= uint32_t{1} << index;
        }
    }
    return mask;
}

void GoalAuthorityTable::approve(
    uint32_t goal_index,
    const std::array<GoalGrantReference, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> &references,
    foundation::TransitionStamp stamp) {
    auto &slot = slots_[goal_index];
    for (uint32_t index = 0; index < slot.count; ++index) {
        requirements_[requirement_index(goal_index, index)].dependency = references[index];
    }
    slot.phase = GoalAuthorityPhase::authorized;
    slot.stamp = stamp;
}

void GoalAuthorityTable::pause(uint32_t goal_index, foundation::TransitionStamp stamp) {
    slots_[goal_index].phase = GoalAuthorityPhase::paused;
    slots_[goal_index].stamp = stamp;
}

bool GoalAuthorityTable::authorized(uint32_t goal_index, uint64_t goal_id) const {
    return contains(goal_index, goal_id) &&
           slots_[goal_index].phase == GoalAuthorityPhase::authorized;
}

void GoalAuthorityTable::clear(uint32_t goal_index) {
    if (goal_index < limits_.goals) {
        slots_[goal_index] = {};
    }
}

void GoalAuthorityTable::clear_all() {
    for (uint32_t index = 0; index < limits_.goals; ++index) {
        clear(index);
    }
}

} // namespace ayni::work
