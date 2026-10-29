#include "kernel/service/model/work_model.h"

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
        exporting.model.catalogs.capabilities.find(input->capability_id, input->capability_version);
    if (capability.kind != catalog::CapabilityRegistry::Lookup::Kind::found) {
        result =
            capability.kind == catalog::CapabilityRegistry::Lookup::Kind::unknown_id
                ? authority::GrantCheck{AYNI_NOT_FOUND, AYNI_REASON_CAPABILITY_UNKNOWN}
                : authority::GrantCheck{AYNI_UNSUPPORTED_VERSION, AYNI_REASON_CAPABILITY_VERSION};
        return result.status;
    }
    const auto &registered = exporting.model.catalogs.capabilities.at(capability.index);
    result.status = exporting.model.goal_authority.stage(
        exporting.record, registered.id, registered.version,
        exporting.model.catalogs.capabilities.domain_view(capability.index), input->scope,
        exporting.model.catalogs.capabilities.business_view(capability.index));
    result.reason = result.status == AYNI_CAPACITY_EXCEEDED  ? AYNI_REASON_GOAL_REQUIREMENT_CAPACITY
                    : result.status == AYNI_INVALID_ARGUMENT ? AYNI_REASON_GOAL_REQUIREMENT_INPUT
                                                             : AYNI_REASON_NONE;
    return result.status;
}

} // namespace

// Runtime admission additionally requires an authorized retained parent;
// immutable Scope coverage remains with the work owner (ADR-0038).
bool WorkModel::parent_covers_authority(uint64_t parent_id,
                                        const work::GoalAuthorityRecord &record) const {
    if (parent_id == 0U) {
        return true;
    }
    uint32_t parent_index = 0;
    if (!goals.find(parent_id, parent_index) ||
        !goal_authority.authorized(parent_index, parent_id)) {
        return false;
    }
    return work::goal_ceiling_covers(goal_authority.at(parent_index), record);
}

authority::GrantCheck WorkModel::check_parent_authority(uint32_t parent_index, uint64_t now_us,
                                                        uint64_t durable_seq) const {
    if (parent_index == work::GoalTable::no_parent) {
        return {AYNI_OK, AYNI_REASON_NONE};
    }
    const bool available = goals.visit_lineage(parent_index, [&](uint32_t at) {
        const auto &parent = goals.at(at);
        return work::GoalTable::is_live(parent.lifecycle) && !work::GoalTable::is_held(parent) &&
               (parent.expires_us == work::no_time || parent.expires_us > now_us);
    });
    return available ? check_goal_authority(parent_index, now_us, durable_seq)
                     : authority::GrantCheck{AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_PARENT};
}

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
        const auto status =
            call_out([&] { return type.export_requirements(type.ctx, create.parameters, &sink); });
        if (exporting.result.status != AYNI_OK) {
            return exporting.result;
        }
        if (status != AYNI_OK) {
            return {status < AYNI_OK ? status : AYNI_INVALID_ARGUMENT, AYNI_REASON_NONE};
        }
    }
    // Exporters can consume time without mutating Kernel state. Select against
    // the post-export clock, not the original trigger-schedule anchor.
    now_us = clock.now_us();
    if (!parent_covers_authority(create.parent_goal_id, out)) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_PARENT};
    }
    if (create.parent_goal_id != 0U) {
        uint32_t parent = 0;
        if (!goals.find(create.parent_goal_id, parent)) {
            return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_PARENT};
        }
        const auto check = check_goal_authority(parent, now_us, durable_seq);
        if (check.status != AYNI_OK) {
            return check;
        }
    }
    for (uint32_t index = 0; index < out.count; ++index) {
        auto &requirement = out.requirements[index];
        for (uint32_t at = 0; at < grants.size(); ++at) {
            const auto &grant = grants.at(at);
            if (grant.subject_instance_id != create.owner_instance_id ||
                (requirement.dependency.grant_id != 0U &&
                 grant.grant_id >= requirement.dependency.grant_id) ||
                !grants.covers_saved_scope(
                    at, requirement.capability_id, requirement.capability_version,
                    requirement.source_domain, requirement.scope, requirement.business_contract) ||
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
                                                   uint64_t now_us, uint64_t durable_seq,
                                                   uint64_t goal_id) const {
    uint32_t index = 0;
    if (!grants.find(requirement.dependency.grant_id, index)) {
        return {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
    }
    const auto check = check_grant(index, now_us, durable_seq, goal_id);
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
    authority::GrantCheck result{AYNI_OK, AYNI_REASON_NONE};
    goals.visit_lineage(goal_index, [&](uint32_t at) {
        work::GoalAuthorityRecord record{};
        if (!goal_authority.read(at, goals.at(at).goal_id, record)) {
            result = {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
            return false;
        }
        if (record.phase != work::GoalAuthorityPhase::authorized) {
            result = {AYNI_DENIED, record.phase == work::GoalAuthorityPhase::paused
                                       ? AYNI_REASON_GOAL_AUTHORITY_PAUSED
                                       : AYNI_REASON_GOAL_AUTHORITY_MISSING};
            return false;
        }
        if (work::replacement_binding_pending(record.binding, goals.at(at).created.commit_seq,
                                              durable_seq)) {
            result = {AYNI_BUSY, AYNI_REASON_GOAL_AUTHORITY_NOT_DURABLE};
            // Still prove the chain: a revoke/expiry is a denial even while
            // the replacement fact itself is waiting for durability.
        }
        if (goals.at(at).parent_index != work::GoalTable::no_parent) {
            const auto parent = goals.at(at).parent_index;
            const auto parent_id = goals.at(parent).goal_id;
            work::GoalAuthorityRecord parent_record{};
            if (!goal_authority.read(parent, parent_id, parent_record) ||
                record.binding.parent.goal_id != parent_id ||
                record.binding.parent.commit_seq != parent_record.binding.established_seq) {
                result = {AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_PARENT};
                return false;
            }
        }
        for (uint32_t index = 0; index < record.count; ++index) {
            const auto check =
                check_requirement(record.requirements[index], now_us, durable_seq, record.goal_id);
            if (check.status != AYNI_OK) {
                result = check;
                return false;
            }
        }
        return true;
    });
    return result;
}

void WorkModel::pause_goal_authority(uint32_t index) {
    const auto stamp = transitions.next();
    goal_authority.pause(index, stamp);
    goals.authority_changed(index, stamp);
    for (uint32_t at = 0; at < tasks.size(); ++at) {
        const auto &task = tasks.at(at);
        if (task.goal_index == index && work::TaskTable::is_live(task.lifecycle) &&
            task_authority.pause(task.slot, stamp)) {
            tasks.authority_changed(at, stamp);
        }
    }
}

} // namespace ayni::kernel
