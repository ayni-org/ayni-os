// Cross-owner checks for Task-owned, lazily acquired tool authority (ADR-0085).
// Call owns the exact selection; Grant owns the original full Scope; Task owns
// the currently selected reference. None can reinterpret another owner's bytes.
#include "kernel/service/model/work_model.h"
namespace ayni::kernel {
bool WorkModel::tool_policy_allows(uint32_t goal_index, const catalog::Binding &binding) const {
    for (uint32_t at = goal_index; at != work::GoalTable::no_parent;
         at = goals.at(at).parent_index) {
        const auto &policy = goals.at(at).tools;
        if (policy.authorization == AYNI_TOOLS_ON_DEMAND &&
            !policy.contains({binding.id.data(), binding.id.size()}, binding.version)) {
            return false;
        }
    }
    return true;
}
bool WorkModel::child_tool_policy_allowed(uint64_t parent_id,
                                          const catalog::ToolPolicy &policy) const {
    if (!parent_id) {
        return true;
    }
    uint32_t parent = 0;
    if (!goals.find(parent_id, parent)) {
        return false;
    }
    for (uint32_t at = parent; at != work::GoalTable::no_parent; at = goals.at(at).parent_index) {
        const auto &ceiling = goals.at(at).tools;
        if (ceiling.authorization != AYNI_TOOLS_ON_DEMAND) {
            continue;
        }
        for (uint32_t tool = 0; tool < policy.count; ++tool) {
            const auto &reference = policy.tools[tool];
            bool found = false;
            for (uint32_t allowed = 0; allowed < ceiling.count; ++allowed) {
                const auto &candidate = ceiling.tools[allowed];
                found = found || (candidate.binding_id.equals(
                                      {reference.binding_id.data(), reference.binding_id.size()}) &&
                                  candidate.binding_version == reference.binding_version &&
                                  candidate.role == reference.role);
            }
            if (!found) {
                return false;
            }
        }
    }
    return true;
}
// A source is historical evidence, not an assertion that its original Grant is
// still usable. Revocation cannot remove its Scope while a live Task refers to it.
bool WorkModel::tool_authority_source(uint64_t task_id, const work::TaskToolReference &reference,
                                      uint32_t &call_index, uint32_t &grant_index) const {
    if (!calls.find(reference.source_call_id, call_index)) {
        return false;
    }
    const auto &id = calls.identity(call_index);
    if (id.task_id != task_id || !id.authorization.grant_id ||
        calls.contract(call_index).category != catalog::CallClass::tool ||
        !grants.find(id.authorization.grant_id, grant_index)) {
        return false;
    }
    const auto &source = grants.at(grant_index);
    uint32_t task_index = 0;
    if (!tasks.find(task_id, task_index) ||
        (source.bound_goal_id && source.bound_goal_id != tasks.at(task_index).goal_id)) {
        return false;
    }
    return id.authorization.revision == authority::grant_active_revision &&
           source.subject_instance_id == id.owner_instance_id &&
           source.capability_id.equals({id.capability_id.data(), id.capability_id.size()}) &&
           source.capability_version == id.capability_version &&
           grants.covers_saved_scope(grant_index, id.capability_id, id.capability_version,
                                     calls.domain(call_index), grants.scope(grant_index),
                                     calls.contract(call_index).business_contract) &&
           grants.active_at(grant_index, calls.state(call_index).created.transition.commit_seq);
}
// Reuse the original full ceiling and implementation identity. A new usable
// Grant elsewhere in the catalog is never an implicit rebind of an old Task.
authority::GrantCheck WorkModel::check_tool_authority(uint32_t task_index,
                                                      const work::TaskToolReference &reference,
                                                      uint64_t now_us, uint64_t durable_seq) const {
    const auto &task = tasks.at(task_index);
    uint32_t source_call = 0, source_grant = 0, selected = 0;
    if (!tool_authority_source(task.task_id, reference, source_call, source_grant) ||
        !grants.find(reference.selected.grant_id, selected) ||
        grants.at(selected).subject_instance_id != task.owner_instance_id) {
        return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
    }
    const auto &id = calls.identity(source_call);
    uint32_t binding = 0;
    if (!catalogs.bindings.find({id.binding_id.data(), id.binding_id.size()}, id.binding_version,
                                binding) ||
        !catalog::BindingRegistry::same_selection(id.binding(), catalogs.bindings.at(binding))) {
        return {AYNI_UNSUPPORTED_VERSION, AYNI_REASON_GRANT_CATALOG_CHANGED};
    }
    // The source proof above already checked its own full Scope. An explicit
    // replacement still has to cover that same ceiling (ADR-0085).
    if (!tool_policy_allows(task.goal_index, catalogs.bindings.at(binding)) ||
        (selected != source_grant &&
         !grants.covers_saved_scope(selected, id.capability_id, id.capability_version,
                                    calls.domain(source_call), grants.scope(source_grant),
                                    calls.contract(source_call).business_contract))) {
        return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
    }
    const auto check = check_grant(selected, now_us, durable_seq, task.goal_id);
    if (check.status != AYNI_OK) {
        return check;
    } // Preserve revoke/expiry/compatibility causes.
    return grants.at(selected).revision == reference.selected.revision
               ? check
               : authority::GrantCheck{AYNI_DENIED, AYNI_REASON_GOAL_AUTHORITY_MISSING};
}
// Registration is frozen for this boot. Catalog or subject absence holds a
// ready Task without persisting a new authorization pause (C-AUTH-020).
bool WorkModel::tool_references_compatible(const work::Task &task) const {
    if (!task_authority.contains(task.slot, task.task_id)) {
        return true;
    }
    const auto record = task_authority.at(task.slot);
    for (uint32_t at = 0; at < record.tool_count; ++at) {
        const auto &reference = record.tools[at];
        uint32_t selected = 0;
        if (grants.find(reference.selected.grant_id, selected) &&
            grants.has_compatibility_hold(selected)) {
            return false;
        }
        uint32_t call = 0;
        if (!calls.find(reference.source_call_id, call)) {
            continue;
        } // Missing facts reach ordinary refusal.
        const auto &id = calls.identity(call);
        uint32_t binding = 0;
        if (!catalogs.bindings.find({id.binding_id.data(), id.binding_id.size()},
                                    id.binding_version, binding) ||
            !catalog::BindingRegistry::same_selection(id.binding(),
                                                      catalogs.bindings.at(binding))) {
            return false;
        }
    }
    return true;
}
bool WorkModel::has_unusable_tool_authority(uint32_t goal_index, uint64_t now_us,
                                            uint64_t durable_seq) const {
    for (uint32_t at = 0; at < tasks.size(); ++at) {
        const auto &task = tasks.at(at);
        if (task.goal_index == goal_index && work::TaskTable::is_live(task.lifecycle) &&
            task_authority.contains(task.slot, task.task_id)) {
            const auto record = task_authority.at(task.slot);
            const auto check = check_task_authority(at, now_us, durable_seq);
            if (record.tool_count && check.status != AYNI_OK && check.status != AYNI_BUSY) {
                return true;
            }
        }
    }
    return false;
}
void WorkModel::pause_task_authority(uint32_t task_index, uint64_t now_us, uint64_t durable_seq) {
    const auto &task = tasks.at(task_index);
    if (check_goal_authority(task.goal_index, now_us, durable_seq).status != AYNI_OK) {
        pause_goal_authority(task.goal_index);
        return;
    }
    if (task_authority.at(task.slot).phase == work::TaskAuthorityPhase::paused) {
        return;
    }
    const auto stamp = transitions.next();
    if (task_authority.pause(task.slot, stamp)) {
        tasks.authority_changed(task_index, stamp);
    }
}
// The explicit management command acts on the complete cohort at admission time.
// Stage every choice before publication; source Calls and their original Grants
// remain immutable upper-bound anchors even after selecting a replacement.
authority::GrantCheck WorkModel::stage_tool_reauthorization(uint32_t task_index, uint64_t now_us,
                                                            uint64_t durable_seq) {
    const auto &task = tasks.at(task_index);
    const auto record = task_authority.at(task.slot);
    std::array<work::AuthorityReference, AYNI_TASK_STEP_CAPACITY_MAX> selections{};
    for (uint32_t at = 0; at < record.tool_count; ++at) {
        uint32_t source_call = 0, source_grant = 0;
        if (!tool_authority_source(task.task_id, record.tools[at], source_call, source_grant)) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
        }
        const auto &id = calls.identity(source_call);
        uint32_t binding = 0;
        if (!catalogs.bindings.find({id.binding_id.data(), id.binding_id.size()},
                                    id.binding_version, binding) ||
            !catalog::BindingRegistry::same_selection(id.binding(),
                                                      catalogs.bindings.at(binding)) ||
            !tool_policy_allows(task.goal_index, catalogs.bindings.at(binding))) {
            return {AYNI_UNSUPPORTED_VERSION, AYNI_REASON_GRANT_CATALOG_CHANGED};
        }
        std::optional<uint32_t> selected;
        for (uint32_t index = 0; index < grants.size(); ++index) {
            const auto &grant = grants.at(index);
            if (grant.subject_instance_id != task.owner_instance_id ||
                !grants.covers_saved_scope(index, id.capability_id, id.capability_version,
                                           calls.domain(source_call), grants.scope(source_grant),
                                           calls.contract(source_call).business_contract) ||
                check_grant(index, now_us, durable_seq, task.goal_id).status != AYNI_OK) {
                continue;
            }
            if (!selected || authority::prefer_admitted_grant(grant, grants.at(*selected))) {
                selected = index;
            }
        }
        if (!selected) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
        }
        const auto &grant = grants.at(*selected);
        selections[at] = {grant.grant_id, grant.revision};
    }
    task_authority.stage_tool_references(task.slot, selections);
    return {AYNI_OK, AYNI_REASON_NONE};
}
} // namespace ayni::kernel
