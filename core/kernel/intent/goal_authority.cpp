#include "kernel/intent/work_model.h"

#include "authority/scope/scope.h"
#include "catalog/abi_checks.h"

namespace ayni::kernel {
namespace {

// One call's scratch views and sticky failure; no live work changes here.
struct RequirementExport {
    WorkModel &model;
    work::GoalAuthorityRecord &record;
    authority::GrantCheck result{AYNI_OK, AYNI_REASON_NONE};
};

ayni_status_t emit_requirement(void *ctx, const ayni_goal_requirement_v1 *input) {
    auto &exporting = *static_cast<RequirementExport *>(ctx);
    if (exporting.result.status != AYNI_OK) {
        return exporting.result.status;
    }
    auto &result = exporting.result;
    result = {AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_REQUIREMENT_INPUT};
    if (input == nullptr) {
        return result.status;
    }
    result.status = catalog::check_struct_size(input->struct_size, sizeof(*input), sizeof(*input));
    if (result.status != AYNI_OK) {
        result.reason = result.status == AYNI_INVALID_ARGUMENT ? result.reason : AYNI_REASON_NONE;
        return result.status;
    }
    if (!catalog::CatalogId::is_valid(input->capability_id) || input->capability_version == 0U) {
        result = {AYNI_INVALID_ARGUMENT, AYNI_REASON_GOAL_REQUIREMENT_INPUT};
        return result.status;
    }
    const auto capability =
        exporting.model.capabilities.find(input->capability_id, input->capability_version);
    if (capability.kind != catalog::CapabilityRegistry::Lookup::Kind::found) {
        result =
            capability.kind == catalog::CapabilityRegistry::Lookup::Kind::unknown_id
                ? authority::GrantCheck{AYNI_NOT_FOUND, AYNI_REASON_CAPABILITY_UNKNOWN}
                : authority::GrantCheck{AYNI_UNSUPPORTED_VERSION, AYNI_REASON_CAPABILITY_VERSION};
        return result.status;
    }
    const auto &registered = exporting.model.capabilities.at(capability.index);
    result.status = exporting.model.goal_authority.stage(
        exporting.record, registered.id, registered.version,
        exporting.model.capabilities.domain_view(capability.index), input->scope);
    result.reason = result.status == AYNI_CAPACITY_EXCEEDED  ? AYNI_REASON_GOAL_REQUIREMENT_CAPACITY
                    : result.status == AYNI_INVALID_ARGUMENT ? AYNI_REASON_GOAL_REQUIREMENT_INPUT
                                                             : AYNI_REASON_NONE;
    return result.status;
}

// A child may attenuate each complete parent requirement, never union them.
bool parent_covers(const WorkModel &model, uint64_t parent_id,
                   const work::GoalAuthorityRecord &record) {
    if (parent_id == 0U) {
        return true;
    }
    uint32_t parent_index = 0;
    if (!model.goals.find(parent_id, parent_index) ||
        !model.goal_authority.authorized(parent_index, parent_id)) {
        return false;
    }
    const auto parent = model.goal_authority.at(parent_index);
    for (uint32_t index = 0; index < record.count; ++index) {
        const auto &child = record.requirements[index];
        bool covered = false;
        for (uint32_t at = 0; at < parent.count && !covered; ++at) {
            const auto &ceiling = parent.requirements[at];
            if (ceiling.capability_id.equals(
                    {child.capability_id.data(), child.capability_id.size()}) &&
                ceiling.capability_version == child.capability_version) {
                const auto relation =
                    authority::covers(child.source_domain, ceiling.scope, child.scope);
                covered = relation.status == AYNI_OK && relation.covered;
            }
        }
        if (!covered) {
            return false;
        }
    }
    return true;
}

} // namespace

// Export temporary caller views once, then select immutable, durable Grant
// identities. A refusal changes neither IDs nor transitions nor live slots.
authority::GrantCheck WorkModel::prepare_goal_authority(const catalog::GoalType &type,
                                                        const GoalCreate &create, uint64_t now_us,
                                                        uint64_t durable_seq,
                                                        work::GoalAuthorityRecord &out,
                                                        OwnerClock clock) {
    RequirementExport exporting{*this, out};
    if (type.export_requirements != nullptr) {
        const ayni_goal_requirement_sink_v1 sink{sizeof(sink), &exporting, emit_requirement};
        calling_out = true;
        const auto status = type.export_requirements(type.ctx, create.parameters, &sink);
        calling_out = false;
        if (exporting.result.status != AYNI_OK) {
            return exporting.result;
        }
        if (status != AYNI_OK) {
            return {status < AYNI_OK ? status : AYNI_INVALID_ARGUMENT, AYNI_REASON_NONE};
        }
    }
    // Exporters can consume time without mutating Kernel state. Select against
    // the post-export clock, not the original trigger-schedule anchor.
    now_us = clock.now_us != nullptr ? clock.now_us(clock.ctx) : now_us;
    if (!parent_covers(*this, create.parent_goal_id, out)) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_PARENT};
    }
    for (uint32_t index = 0; index < out.count; ++index) {
        auto &requirement = out.requirements[index];
        for (uint32_t at = 0; at < grants.size(); ++at) {
            const auto &grant = grants.at(at);
            if (grant.subject_instance_id != create.owner_instance_id ||
                (requirement.dependency.grant_id != 0U &&
                 grant.grant_id >= requirement.dependency.grant_id) ||
                !grants.covers_saved_scope(at, requirement.capability_id,
                                           requirement.capability_version,
                                           requirement.source_domain, requirement.scope) ||
                check_grant(at, now_us, durable_seq).status != AYNI_OK) {
                continue;
            }
            requirement.dependency = {grant.grant_id, grant.revision};
        }
        if (requirement.dependency.grant_id == 0U) {
            out.phase = work::GoalAuthorityPhase::proposed;
        }
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Check the originally selected dependency. Never substitute another Grant
// after revocation, expiry or catalog drift (ADR-0037 decision 6).
authority::GrantCheck WorkModel::check_requirement(const work::GoalRequirement &requirement,
                                                   uint64_t now_us, uint64_t durable_seq) const {
    uint32_t index = 0;
    if (!grants.find(requirement.dependency.grant_id, index)) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
    }
    const auto check = check_grant(index, now_us, durable_seq);
    if (check.status != AYNI_OK) {
        return check;
    }
    if (grants.at(index).revision != requirement.dependency.revision) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// The saved ceiling's phase and each selected chain independently constrain
// consumption. Ancestor authority is checked at the same boundary.
authority::GrantCheck WorkModel::check_goal_authority(uint32_t goal_index, uint64_t now_us,
                                                      uint64_t durable_seq) const {
    for (uint32_t at = goal_index; at != work::GoalTable::no_parent;
         at = goals.at(at).parent_index) {
        work::GoalAuthorityRecord record{};
        if (!goal_authority.read(at, goals.at(at).goal_id, record)) {
            return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
        }
        if (record.phase != work::GoalAuthorityPhase::authorized) {
            return {AYNI_DENIED, record.phase == work::GoalAuthorityPhase::paused
                                     ? AYNI_REASON_GOAL_AUTHORITY_PAUSED
                                     : AYNI_REASON_GOAL_AUTHORITY_MISSING};
        }
        for (uint32_t index = 0; index < record.count; ++index) {
            const auto check = check_requirement(record.requirements[index], now_us, durable_seq);
            if (check.status != AYNI_OK) {
                return check;
            }
        }
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

void WorkModel::pause_goal_authority(uint32_t index) {
    const auto stamp = transitions.next();
    goal_authority.pause(index, stamp);
    goals.authority_changed(index, stamp);
    auto work = tables();
    work::settle_goal_authority(work, index, stamp);
    due_work_changed = true;
}

} // namespace ayni::kernel
