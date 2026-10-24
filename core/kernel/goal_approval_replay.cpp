#include "kernel/work_replay.h"

#include "authority/grant/grant_records.h"

#include <cstring>

namespace ayni::kernel {
namespace {

bool same_bytes(ayni_bytes_v1 left, ayni_bytes_v1 right) {
    return left.size == right.size && std::memcmp(left.data, right.data, left.size) == 0;
}

// A newly issued Grant is exactly the saved item, not a wider covering Grant.
template <typename Grant>
bool matches_requirement(const Grant &grant, ayni_bytes_v1 domain, ayni_bytes_v1 scope,
                         const work::GoalRequirement &need) {
    return grant.capability_id.equals({need.capability_id.data(), need.capability_id.size()}) &&
           grant.capability_version == need.capability_version &&
           same_bytes(domain, need.source_domain) && same_bytes(scope, need.scope);
}

} // namespace

// Capture the original proposal before any approval record changes it. New
// Grant bodies must each match a missing item; metadata later proves a bijection.
bool WorkReplay::pair_approval_grant(const authority::GrantRecord &record,
                                     foundation::TransitionStamp stamp) {
    if (!pairing_.approval) {
        uint32_t index = 0;
        work::GoalAuthorityRecord original{};
        if (batch_seq_ == stamp.commit_seq || pairing_.goal || pairing_.goal_authority ||
            pairing_.goal_authority_seq == stamp.commit_seq ||
            !model_.goals.find(record.grant.bound_goal_id, index) ||
            model_.goals.at(index).lifecycle != AYNI_GOAL_PROPOSED ||
            model_.goals.at(index).commit_seq >= stamp.commit_seq ||
            !model_.goal_authority.read(index, record.grant.bound_goal_id, original) ||
            original.phase != work::GoalAuthorityPhase::proposed) {
            return false;
        }
        pairing_.approval =
            ApprovalPair{ApprovalPair::Stage::grants, record.grant.bound_goal_id, index,
                         model_.goals.at(index).revision, work::missing_requirement_mask(original)};
    }
    auto &pair = *pairing_.approval;
    if (pair.stage != ApprovalPair::Stage::grants || record.grant.bound_goal_id != pair.goal_id ||
        record.grant.subject_instance_id != model_.goals.at(pair.goal_index).owner_instance_id) {
        return false;
    }
    const auto original = model_.goal_authority.at(pair.goal_index);
    uint32_t missing = 0;
    bool matches = false;
    for (uint32_t index = 0; index < original.count; ++index) {
        const auto &need = original.requirements[index];
        if (need.dependency.grant_id == 0U) {
            ++missing;
            matches = matches || matches_requirement(record.grant, record.grant.source_domain,
                                                     record.scope, need);
        }
    }
    if (!matches || pair.count >= missing) {
        return false;
    }
    pair.grant_ids[pair.count++] = record.grant_id;
    return true;
}

// Only approval may activate a proposal. Trigger timestamps are not stored;
// replay checks the initial cursor facts and recovery reanchors the schedule.
bool WorkReplay::pair_approval_state(uint32_t index, const work::GoalState &state) {
    const auto &goal = model_.goals.at(index);
    const bool activating =
        goal.lifecycle == AYNI_GOAL_PROPOSED && state.lifecycle == AYNI_GOAL_ACTIVE;
    if (!pairing_.approval) {
        return !activating;
    }
    auto &pair = *pairing_.approval;
    if (!activating || index != pair.goal_index || pair.stage != ApprovalPair::Stage::grants ||
        pair.revision == UINT32_MAX || state.revision != pair.revision + 1U ||
        state.last_task_id != 0U || state.taken != 0U || state.waiting != 0U ||
        state.occurrences_skipped != 0U || state.skip_reason != AYNI_REASON_NONE ||
        state.cancel_revision != 0U || state.expiry_pending != (goal.expires_us != work::no_time) ||
        ((goal.trigger.kind == AYNI_TRIGGER_NONE ||
          goal.trigger.kind == AYNI_TRIGGER_LOCAL_INPUT) &&
         state.next_scheduled)) {
        return false;
    }
    pair.stage = ApprovalPair::Stage::state;
    return true;
}

// New dependencies must use every issued Grant exactly once. Existing choices
// remain byte-for-byte identities; immutable ceiling comparison belongs to work.
bool WorkReplay::pair_approval_metadata(uint32_t index, const work::GoalAuthorityRecord &record) {
    work::GoalAuthorityRecord original{};
    const bool existed = model_.goal_authority.read(index, record.goal_id, original);
    const bool approving = existed && original.phase == work::GoalAuthorityPhase::proposed &&
                           record.phase == work::GoalAuthorityPhase::authorized;
    if (!pairing_.approval) {
        if (approving || (existed && original.phase == work::GoalAuthorityPhase::proposed &&
                          record.phase != original.phase)) {
            return false;
        }
        // E has no reference replacement command. Autonomous pauses preserve
        // every selected or absent identity; F must add its own paired proof.
        for (uint32_t at = 0; existed && at < original.count && at < record.count; ++at) {
            const auto &before = original.requirements[at].dependency;
            const auto &after = record.requirements[at].dependency;
            if (before.grant_id != after.grant_id || before.revision != after.revision) {
                return false;
            }
        }
        return true;
    }
    auto &pair = *pairing_.approval;
    if (!approving || index != pair.goal_index || pair.stage != ApprovalPair::Stage::state ||
        record.count != original.count) {
        return false;
    }
    uint32_t used = 0;
    for (uint32_t item = 0; item < record.count; ++item) {
        const auto &before = original.requirements[item].dependency;
        const auto &after = record.requirements[item].dependency;
        if (before.grant_id != 0U) {
            if (after.grant_id != before.grant_id || after.revision != before.revision) {
                return false;
            }
            continue;
        }
        uint32_t grant_index = 0;
        if (after.revision != authority::grant_active_revision ||
            !model_.grants.find(after.grant_id, grant_index) ||
            !matches_requirement(model_.grants.at(grant_index),
                                 model_.grants.source_domain(grant_index),
                                 model_.grants.scope(grant_index), original.requirements[item])) {
            return false;
        }
        uint32_t at = 0;
        while (at < pair.count && pair.grant_ids[at] != after.grant_id) {
            ++at;
        }
        if (at == pair.count || (used & (uint32_t{1} << at)) != 0U) {
            return false;
        }
        used |= uint32_t{1} << at;
    }
    if (used != (uint32_t{1} << pair.count) - 1U) {
        return false;
    }
    pair.stage = ApprovalPair::Stage::metadata;
    return true;
}

// Close the batch only when the canonical identity proves the original mask,
// revision and every issued Grant. Keep the closed proof to reject trailing work.
bool WorkReplay::pair_approval_command(const CommandMapping &mapping, ayni_bytes_v1 canonical) {
    ApprovalCommand request{};
    if (!pairing_.approval || !read_approval_command(canonical, request)) {
        return false;
    }
    const auto &pair = *pairing_.approval;
    if (pair.stage != ApprovalPair::Stage::metadata || mapping.object.id != pair.goal_id ||
        request.expected_revision != pair.revision ||
        request.approval.missing_mask != pair.missing_mask ||
        !matches_goal_approval(mapping, canonical)) {
        return false;
    }
    pairing_.approval->stage = ApprovalPair::Stage::complete;
    return true;
}

// Historical command retention can outlive the target or terminal Grants.
// Every retained fact must still agree; missing reclaimed history is not recreated.
bool WorkReplay::matches_goal_approval(const CommandMapping &mapping,
                                       ayni_bytes_v1 canonical) const {
    ApprovalCommand request{};
    const auto *binding = model_.principals.binding();
    if (!read_approval_command(canonical, request) || binding == nullptr ||
        mapping.principal_id != binding->principal_id ||
        binding->stamp.commit_seq >= mapping.commit_seq ||
        model_.agents.find_instance(request.approval.original.owner_instance_id) == nullptr) {
        return false;
    }
    uint32_t index = 0;
    if (!model_.goals.find(mapping.object.id, index)) {
        return true;
    }
    const auto &goal = model_.goals.at(index);
    work::GoalAuthorityRecord record{};
    if (goal.revision < mapping.object.revision || goal.commit_seq < mapping.commit_seq ||
        !matches_goal_creation(request.approval.original, goal, model_.goals.parameters(index)) ||
        !model_.goal_authority.read(index, goal.goal_id, record) ||
        record.phase == work::GoalAuthorityPhase::proposed ||
        record.stamp.commit_seq < mapping.commit_seq ||
        (request.approval.missing_mask >> record.count) != 0U) {
        return false;
    }
    for (uint32_t item = 0; item < record.count; ++item) {
        uint32_t at = 0;
        if (!model_.grants.find(record.requirements[item].dependency.grant_id, at)) {
            continue;
        }
        const auto &grant = model_.grants.at(at);
        if ((request.approval.missing_mask & (uint32_t{1} << item)) == 0U) {
            // Removing a mask bit cannot disguise a retained new Grant as
            // an original reference: those necessarily precede approval.
            if (!model_.grants.active_at(at, mapping.commit_seq)) {
                return false;
            }
            continue;
        }
        if (grant.created_stamp.commit_seq != mapping.commit_seq ||
            grant.bound_goal_id != goal.goal_id ||
            grant.owner_principal_id != mapping.principal_id ||
            grant.subject_instance_id != goal.owner_instance_id ||
            grant.lifetime != request.approval.lifetime ||
            grant.duration_us != request.approval.duration_us ||
            !matches_requirement(grant, model_.grants.source_domain(at), model_.grants.scope(at),
                                 record.requirements[item])) {
            return false;
        }
    }
    return true;
}

} // namespace ayni::kernel
