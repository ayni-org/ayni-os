#include "authority/grant/grant_table.h"

#include "authority/grant/grant_records.h"
#include "authority/scope/scope.h"
#include "foundation/kernel_id.h"

#include <cstring>
#include <limits>

namespace ayni::authority {

using foundation::id_boot_epoch;
using foundation::is_kernel_id;

namespace {

// Reject malformed scalars before unsupported features and all scope/table
// work. Finite deadline addition is bounded before creating an Admission.
GrantCheck creation_input(const NewGrant &grant, uint32_t expected_parent_revision,
                          GrantContext context) {
    const bool limited = grant.lifetime == AYNI_GRANT_CURRENT_BOOT;
    if (grant.subject_instance_id == 0U || grant.owner_principal_id == 0U ||
        grant.owner_binding_revision == 0U || grant.capability_id.size() == 0U ||
        grant.capability_version == 0U ||
        !valid_goal_binding(grant.bound_goal_id, grant.parent_grant_id, grant.flags) ||
        !valid_grant_flags(grant.flags) ||
        !valid_grant_lifetime(grant.lifetime, grant.duration_us) || context.boot_epoch == 0U ||
        context.boot_epoch > foundation::max_boot_epoch ||
        (limited && grant.duration_us > std::numeric_limits<uint64_t>::max() - context.now_us) ||
        (grant.parent_grant_id == 0U ? expected_parent_revision != 0U
                                     : expected_parent_revision == 0U)) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_GRANT_INPUT};
    }
    if (needs_deferred_grant_feature(grant.flags)) {
        return {AYNI_UNSUPPORTED, AYNI_REASON_GRANT_UNSUPPORTED};
    }
    return {AYNI_OK, AYNI_REASON_NONE};
}

// creation_input already checked overflow; both typed and canonical admissions
// start finite lifetime at acceptance, without waiting for durable publication.
uint64_t creation_deadline(const NewGrant &grant, uint64_t now_us) {
    return grant.lifetime == AYNI_GRANT_CURRENT_BOOT ? now_us + grant.duration_us
                                                     : no_grant_deadline_us;
}

bool same_bytes(ayni_bytes_v1 left, ayni_bytes_v1 right) {
    return left.size == right.size &&
           (left.size == 0U || std::memcmp(left.data, right.data, left.size) == 0);
}

bool stamped(foundation::TransitionStamp stamp) {
    return stamp.transition_id != 0U && stamp.commit_seq != 0U;
}

bool after(foundation::TransitionStamp newer, foundation::TransitionStamp older) {
    return newer.transition_id > older.transition_id && newer.commit_seq > older.commit_seq;
}

bool same_stamp(foundation::TransitionStamp left, foundation::TransitionStamp right) {
    return left.transition_id == right.transition_id && left.commit_seq == right.commit_seq;
}

bool old_boot(const Grant &grant, uint64_t boot_epoch) {
    return grant.lifetime == AYNI_GRANT_CURRENT_BOOT &&
           (id_boot_epoch(grant.grant_id) != boot_epoch ||
            grant.deadline_us == no_grant_deadline_us);
}

bool catalog_hold(ayni_reason_t reason) {
    return reason == AYNI_REASON_CAPABILITY_UNKNOWN || reason == AYNI_REASON_CAPABILITY_VERSION ||
           reason == AYNI_REASON_GRANT_CATALOG_CHANGED;
}

bool compatibility_hold(ayni_reason_t reason) {
    return catalog_hold(reason) || reason == AYNI_REASON_GRANT_SUBJECT;
}

GrantCheck held(ayni_reason_t reason) {
    if (reason == AYNI_REASON_CAPABILITY_UNKNOWN) {
        return {AYNI_NOT_FOUND, reason};
    }
    if (reason == AYNI_REASON_CAPABILITY_VERSION || reason == AYNI_REASON_GRANT_CATALOG_CHANGED) {
        return {AYNI_UNSUPPORTED_VERSION, reason};
    }
    return {AYNI_DENIED, reason};
}

bool lifecycle_state(const GrantRecord &record, foundation::TransitionStamp created,
                     foundation::TransitionStamp stamp, uint32_t revision,
                     ayni_grant_lifecycle_t lifecycle) {
    if (!stamped(created) || !stamped(stamp)) {
        return false;
    }
    return lifecycle == AYNI_GRANT_ACTIVE
               ? revision == grant_active_revision && same_stamp(created, stamp)
               : revision == grant_terminal_revision && after(stamp, created) &&
                     (lifecycle == AYNI_GRANT_REVOKED ||
                      (lifecycle == AYNI_GRANT_GOAL_ENDED && record.grant.bound_goal_id != 0U) ||
                      (lifecycle == AYNI_GRANT_EXPIRED &&
                       record.grant.lifetime == AYNI_GRANT_CURRENT_BOOT));
}

void end_by_goal(Grant &grant, foundation::TransitionStamp stamp) {
    grant.lifecycle = AYNI_GRANT_GOAL_ENDED;
    grant.revision = grant_terminal_revision;
    grant.stamp = stamp;
}

// Callers already admitted the transition/stamp. Root cleanup metadata is
// independent; changing a descendant never changes the root's terminal order.
void revoke_by(Grant &grant, uint64_t root_id, foundation::TransitionStamp stamp) {
    grant.lifecycle = AYNI_GRANT_REVOKED;
    grant.revision = grant_terminal_revision;
    grant.stamp = stamp;
    grant.revoked_by_root_id = root_id;
}

bool cleanup_finished(const Grant &grant) {
    return !is_revocation_root(grant.grant_id, grant.revoked_by_root_id) ||
           grant.cleanup_after_grant_id == grant.cleanup_through_grant_id;
}

bool at_or_after(foundation::TransitionStamp newer, foundation::TransitionStamp older) {
    return same_stamp(newer, older) || after(newer, older);
}

} // namespace

bool terminal(const Grant &grant) {
    return grant.lifecycle == AYNI_GRANT_EXPIRED || grant.lifecycle == AYNI_GRANT_REVOKED ||
           grant.lifecycle == AYNI_GRANT_GOAL_ENDED;
}

uint64_t GrantTable::next_bound_goal_after(uint64_t after, uint64_t through) const {
    uint64_t selected = 0;
    for (uint32_t index = 0; index < size(); ++index) {
        const auto id = at(index).bound_goal_id;
        if (id > after && id <= through && (selected == 0U || id < selected)) {
            selected = id;
        }
    }
    return selected;
}

bool GrantTable::has_active_goal_grants(uint64_t goal_id) const {
    for (uint32_t index = 0; index < size(); ++index) {
        if (at(index).bound_goal_id == goal_id && at(index).lifecycle == AYNI_GRANT_ACTIVE) {
            return true;
        }
    }
    return false;
}

void GrantTable::end_goal_grants(uint64_t goal_id, foundation::TransitionStamp stamp) {
    for (uint32_t index = 0; index < size(); ++index) {
        auto &grant = grants_[index];
        if (grant.bound_goal_id == goal_id && grant.lifecycle == AYNI_GRANT_ACTIVE) {
            end_by_goal(grant, stamp);
        }
    }
}

static_assert(GrantTable::storage_size({AYNI_GRANT_CAPACITY_MAX, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX,
                                        AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX}) <=
                  std::numeric_limits<uint32_t>::max(),
              "configured Grant arenas fit on a 32-bit Host");

GrantTable::GrantTable(void *storage, Limits limits)
    : grants_(storage, limits.capacity),
      gates_(limits.capacity == 0U
                 ? nullptr
                 : static_cast<uint8_t *>(storage) +
                       foundation::BoundedArray<Grant>::storage_size(limits.capacity),
             limits.capacity),
      bytes_(limits.capacity == 0U
                 ? nullptr
                 : static_cast<uint8_t *>(storage) +
                       foundation::BoundedArray<Grant>::storage_size(limits.capacity) +
                       foundation::BoundedArray<uint64_t>::storage_size(limits.capacity)),
      limits_(limits) {}

uint8_t *GrantTable::domain_storage(uint32_t index) const {
    return bytes_ + static_cast<std::size_t>(index) * limits_.scope_max_bytes * grant_value_regions;
}

uint8_t *GrantTable::scope_storage(uint32_t index) {
    return domain_storage(index) + limits_.scope_max_bytes;
}

ayni_bytes_v1 GrantTable::source_domain(uint32_t index) const {
    return {domain_storage(index), at(index).source_domain_size};
}

ayni_bytes_v1 GrantTable::scope(uint32_t index) const {
    return {source_domain(index).data + limits_.scope_max_bytes, at(index).scope_size};
}

bool GrantTable::find(uint64_t grant_id, uint32_t &out_index) const {
    if (grant_id == 0U) {
        return false;
    }
    for (uint32_t index = 0; index < size(); ++index) {
        if (at(index).grant_id == grant_id) {
            out_index = index;
            return true;
        }
    }
    return false;
}

bool GrantTable::has_child(uint64_t grant_id) const {
    for (uint32_t index = 0; index < size(); ++index) {
        if (at(index).parent_grant_id == grant_id) {
            return true;
        }
    }
    return false;
}

// C-AUTH-026 orders cleanup by ID, never physical slot. The inclusive frozen
// frontier also supplies the final watermark when no retained candidate remains.
bool GrantTable::next_id_after(uint64_t after_id, uint64_t through_id, uint32_t &out_index) const {
    uint32_t selected = size();
    for (uint32_t index = 0; index < size(); ++index) {
        const uint64_t id = at(index).grant_id;
        if (id > after_id && id <= through_id &&
            (selected == size() || id < at(selected).grant_id)) {
            selected = index;
        }
    }
    if (selected == size()) {
        return false;
    }
    out_index = selected;
    return true;
}

bool GrantTable::descendant_of(uint32_t index, uint64_t root_id) const {
    // A child-to-root relationship uses at most max depth edges. Check the
    // parent ID before lookup, preserving replay's existing frontier semantics.
    return visit_ancestors(index, limits_.delegation_depth_max, [&](uint32_t ancestor) {
               return at(ancestor).parent_grant_id != root_id;
           }) == Walk::stopped;
}

bool GrantTable::reclaimable(uint32_t index, uint64_t durable_seq) const {
    const Grant &grant = at(index);
    return terminal(grant) && grant.stamp.commit_seq <= durable_seq &&
           grant.cleanup_stamp.commit_seq <= durable_seq &&
           grant.reference_release_seq <= durable_seq && cleanup_finished(grant) &&
           !gated(grant.grant_id) && !has_child(grant.grant_id);
}

void GrantTable::release_reference(uint64_t grant_id, uint64_t commit_seq) {
    uint32_t index = 0;
    if (find(grant_id, index) && grants_[index].reference_release_seq < commit_seq) {
        grants_[index].reference_release_seq = commit_seq;
    }
}

bool GrantTable::covers_saved_scope(uint32_t index, const catalog::CatalogId &capability,
                                    uint32_t version, ayni_bytes_v1 original_domain,
                                    ayni_bytes_v1 requested_scope) const {
    const auto &grant = at(index);
    if (!grant.capability_id.equals({capability.data(), capability.size()}) ||
        grant.capability_version != version || !same_bytes(source_domain(index), original_domain)) {
        return false;
    }
    const auto covered = covers(original_domain, scope(index), requested_scope);
    return covered.status == AYNI_OK && covered.covered;
}

// Classify the same history once: snapshots may show later terminal facts;
// replayed creates must precede every ancestor's terminal transition (ADR-0030).
GrantTable::AncestorHistory GrantTable::history_at(uint32_t index, uint64_t commit_seq,
                                                   std::optional<uint64_t> transition_id) const {
    auto history = AncestorHistory::active;
    const auto walked =
        visit_ancestors(index, limits_.delegation_depth_max + 1U, [&](uint32_t ancestor_index) {
            const Grant &ancestor = at(ancestor_index);
            if (commit_seq <= ancestor.created_stamp.commit_seq ||
                (transition_id && *transition_id <= ancestor.created_stamp.transition_id)) {
                return false;
            }
            if (terminal(ancestor)) {
                if (ancestor.stamp.commit_seq <= commit_seq ||
                    (transition_id && ancestor.stamp.transition_id <= *transition_id)) {
                    return false;
                }
                history = AncestorHistory::later_terminal;
            }
            return true;
        });
    return walked == Walk::root ? history : AncestorHistory::invalid;
}

bool GrantTable::active_at(uint32_t index, uint64_t commit_seq) const {
    return history_at(index, commit_seq) != AncestorHistory::invalid;
}

bool GrantTable::gated(uint64_t grant_id) const {
    for (uint32_t index = 0; index < gates_.size(); ++index) {
        if (gates_[index] == grant_id) {
            return true;
        }
    }
    return false;
}

bool GrantTable::can_gate(uint64_t grant_id) const {
    return is_kernel_id(grant_id) && (gated(grant_id) || !gates_.full());
}

bool GrantTable::install_gate(uint64_t grant_id) {
    return can_gate(grant_id) && (gated(grant_id) || gates_.push_back(grant_id));
}

bool GrantTable::release_gate(uint64_t grant_id) {
    for (uint32_t index = 0; index < gates_.size(); ++index) {
        if (gates_[index] == grant_id) {
            gates_.remove_at(index);
            return true;
        }
    }
    return false;
}

GrantCheck GrantTable::check_revoke(uint64_t grant_id, uint32_t expected_revision) const {
    if (grant_id == 0U || expected_revision == 0U) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_GRANT_INPUT};
    }
    uint32_t index = 0;
    if (!find(grant_id, index)) {
        return {AYNI_NOT_FOUND, AYNI_REASON_GRANT_UNKNOWN};
    }
    return expected_revision == at(index).revision
               ? GrantCheck{AYNI_OK, AYNI_REASON_NONE}
               : GrantCheck{AYNI_REVISION_CONFLICT, AYNI_REASON_GRANT_REVISION};
}

GrantTable::RevokePlan GrantTable::plan_accepted_revoke(uint64_t grant_id) const {
    uint32_t index = 0;
    if (!find(grant_id, index)) {
        return {grant_id, 0, AYNI_INTENT_EFFECT_TARGET_GONE, 0, 0};
    }
    const Grant &grant = at(index);
    if (terminal(grant)) {
        return {grant_id, grant.revision, AYNI_INTENT_EFFECT_NO_CHANGE, 0, 0};
    }
    uint64_t through = grant_id;
    for (uint32_t row = 0; row < size(); ++row) {
        if (at(row).grant_id > through) {
            through = at(row).grant_id;
        }
    }
    return {grant_id, grant_terminal_revision, AYNI_INTENT_EFFECT_CHANGED, 0, through};
}

// C-AUTH-027 reuses this validator for a generated live plan and a decoded
// replay plan. A vanished accepted target succeeds without fabricating a row.
bool GrantTable::apply_revoke(const RevokePlan &plan, foundation::TransitionStamp stamp) {
    const RevokePlan expected = plan_accepted_revoke(plan.grant_id);
    if (!is_kernel_id(plan.grant_id) || !stamped(stamp) || plan.revision != expected.revision ||
        plan.effect != expected.effect ||
        plan.cleanup_after_grant_id != expected.cleanup_after_grant_id ||
        plan.cleanup_through_grant_id != expected.cleanup_through_grant_id) {
        return false;
    }
    uint32_t index = 0;
    if (!find(plan.grant_id, index)) {
        return true; // C-AUTH-027: accepted target disappeared during reload
    }
    Grant &grant = grants_[index];
    if (!after(stamp, grant.stamp) ||
        (stamped(grant.cleanup_stamp) && !after(stamp, grant.cleanup_stamp))) {
        return false;
    }
    if (plan.effect == AYNI_INTENT_EFFECT_CHANGED) {
        revoke_by(grant, grant.grant_id, stamp);
        grant.cleanup_after_grant_id = plan.cleanup_after_grant_id;
        grant.cleanup_through_grant_id = plan.cleanup_through_grant_id;
        grant.cleanup_stamp = stamp;
    }
    return true;
}

// C-AUTH-026 selects the oldest unfinished root, then at most one retained
// ID through its frozen frontier; skipped unrelated rows still advance progress.
bool GrantTable::next_revocation_cleanup(CleanupPlan &out) const {
    uint32_t selected = size();
    for (uint32_t index = 0; index < size(); ++index) {
        const Grant &grant = at(index);
        if (grant.lifecycle == AYNI_GRANT_REVOKED &&
            is_revocation_root(grant.grant_id, grant.revoked_by_root_id) &&
            !cleanup_finished(grant) &&
            (selected == size() || grant.stamp.commit_seq < at(selected).stamp.commit_seq ||
             (grant.stamp.commit_seq == at(selected).stamp.commit_seq &&
              grant.grant_id < at(selected).grant_id))) {
            selected = index;
        }
    }
    if (selected == size()) {
        return false;
    }
    const Grant &root = at(selected);
    uint32_t candidate = size();
    const bool found =
        next_id_after(root.cleanup_after_grant_id, root.cleanup_through_grant_id, candidate);
    const bool revoke = found && at(candidate).lifecycle == AYNI_GRANT_ACTIVE &&
                        descendant_of(candidate, root.grant_id);
    out = {root.grant_id,
           candidate == size() ? root.cleanup_through_grant_id : at(candidate).grant_id,
           root.cleanup_through_grant_id, revoke ? at(candidate).grant_id : 0U};
    return true;
}

// Publish a descendant terminal fact and its root cursor together, only
// after the complete plan/stamp matches. Root terminal ordering stays immutable.
bool GrantTable::apply_cleanup(const CleanupPlan &plan, foundation::TransitionStamp stamp) {
    CleanupPlan expected{};
    if (!next_revocation_cleanup(expected) || plan.root_grant_id != expected.root_grant_id ||
        plan.after_grant_id != expected.after_grant_id ||
        plan.through_grant_id != expected.through_grant_id ||
        plan.revoked_grant_id != expected.revoked_grant_id || !stamped(stamp)) {
        return false;
    }
    uint32_t root_index = 0;
    static_cast<void>(find(plan.root_grant_id, root_index));
    Grant &root = grants_[root_index];
    if (!after(stamp, root.cleanup_stamp)) {
        return false;
    }
    uint32_t child_index = 0;
    if (plan.revoked_grant_id != 0U) {
        static_cast<void>(find(plan.revoked_grant_id, child_index));
        if (!after(stamp, at(child_index).stamp)) {
            return false;
        }
        Grant &child = grants_[child_index];
        revoke_by(child, root.grant_id, stamp);
    }
    root.cleanup_after_grant_id = plan.after_grant_id;
    root.cleanup_stamp = stamp;
    return true;
}

// C-AUTH-022: all paths agree on owner, exact catalog/domain, delegation
// permission and lifetime kind. Live deadlines and stored chronology differ
// in available evidence and stay in their respective admission operations.
GrantCheck GrantTable::check_parent_contract(uint32_t parent_index, const NewGrant &child) const {
    const Grant &parent = at(parent_index);
    const bool compatible =
        parent.bound_goal_id == 0U && parent.allow_delegation &&
        parent.owner_principal_id == child.owner_principal_id &&
        parent.owner_binding_revision == child.owner_binding_revision &&
        parent.capability_version == child.capability_version &&
        parent.capability_id.equals({child.capability_id.data(), child.capability_id.size()}) &&
        same_bytes(source_domain(parent_index), child.source_domain) &&
        (parent.lifetime != AYNI_GRANT_CURRENT_BOOT || child.lifetime == AYNI_GRANT_CURRENT_BOOT);
    return compatible ? GrantCheck{AYNI_OK, AYNI_REASON_NONE}
                      : GrantCheck{AYNI_DENIED, AYNI_REASON_GRANT_PARENT};
}

// Validate a retained parent edge using stored duration/boot evidence and,
// when available, live deadlines. Recovery deliberately has no live deadlines.
bool GrantTable::valid_link(uint32_t child_index, uint32_t parent_index) const {
    const Grant &child = at(child_index);
    const Grant &parent = at(parent_index);
    const NewGrant fact{child.subject_instance_id,
                        child.parent_grant_id,
                        child.owner_principal_id,
                        child.owner_binding_revision,
                        child.capability_id,
                        child.capability_version,
                        source_domain(child_index),
                        nullptr,
                        child.lifetime,
                        child.duration_us,
                        child.allow_delegation ? AYNI_GRANT_ALLOW_DELEGATION : 0U};
    if (check_parent_contract(parent_index, fact).status != AYNI_OK ||
        child.depth != parent.depth + 1U || child.depth > limits_.delegation_depth_max) {
        return false;
    }
    const auto covered =
        covers(source_domain(parent_index), scope(parent_index), scope(child_index));
    if (covered.status != AYNI_OK || !covered.covered) {
        return false;
    }
    if (parent.lifetime == AYNI_GRANT_CURRENT_BOOT) {
        return id_boot_epoch(child.grant_id) == id_boot_epoch(parent.grant_id) &&
               child.duration_us <= parent.duration_us &&
               (child.deadline_us == no_grant_deadline_us ||
                parent.deadline_us == no_grant_deadline_us ||
                child.deadline_us <= parent.deadline_us);
    }
    return true;
}

// Common no-hold boots need no per-reference compatibility walk at all.
// This is one borrowed-query sample, not a second persistent hold counter.
bool GrantTable::has_compatibility_holds() const {
    for (uint32_t index = 0; index < size(); ++index) {
        if (compatibility_hold(at(index).hold_reason)) {
            return true;
        }
    }
    return false;
}

// Static per-boot compatibility is separate from revocation/expiry. The same
// bounded owner walk examines cached reasons; malformed links are left for
// the full consumer check instead of silently suppressing their denial.
bool GrantTable::has_compatibility_hold(uint32_t index) const {
    if (index >= size()) {
        return false;
    }
    return visit_ancestors(index, limits_.delegation_depth_max + 1U, [&](uint32_t at_index) {
               const auto reason = at(at_index).hold_reason;
               return !compatibility_hold(reason);
           }) == Walk::stopped;
}

GrantCheck GrantTable::check_chain(uint32_t index, GrantContext context) const {
    CheckedChain chain{};
    return check_chain(index, context, chain);
}

// Authority failures precede subject failures. Publish the validated indices
// so Kernel can check subjects after this entire walk, preserving that order.
GrantCheck GrantTable::check_chain(uint32_t index, GrantContext context, CheckedChain &out) const {
    if (index >= size()) {
        return {AYNI_NOT_FOUND, AYNI_REASON_GRANT_UNKNOWN};
    }
    CheckedChain chain{};
    GrantCheck failure{AYNI_DENIED, AYNI_REASON_GRANT_PARENT};
    const auto walked =
        visit_ancestors(index, limits_.delegation_depth_max + 1U, [&](uint32_t at_index) {
            // Validate each edge before the parent's state, as in C-AUTH-022.
            if (chain.count != 0U && !valid_link(chain.indices[chain.count - 1U], at_index)) {
                return false;
            }
            const Grant &grant = at(at_index);
            if (grant.bound_goal_id != 0U && grant.bound_goal_id != context.goal_id) {
                failure = {AYNI_DENIED, AYNI_REASON_GRANT_GOAL};
                return false;
            }
            if (gated(grant.grant_id) || grant.lifecycle == AYNI_GRANT_REVOKED) {
                failure = {AYNI_DENIED, AYNI_REASON_GRANT_REVOKED};
                return false;
            }
            if (grant.lifecycle == AYNI_GRANT_GOAL_ENDED) {
                failure = {AYNI_DENIED, AYNI_REASON_GRANT_GOAL};
                return false;
            }
            if (grant.hold_reason != AYNI_REASON_NONE) {
                failure = held(grant.hold_reason);
                return false;
            }
            if (grant.lifecycle == AYNI_GRANT_EXPIRED || old_boot(grant, context.boot_epoch) ||
                (grant.lifetime == AYNI_GRANT_CURRENT_BOOT &&
                 context.now_us >= grant.deadline_us)) {
                failure = {AYNI_DENIED, AYNI_REASON_GRANT_EXPIRED};
                return false;
            }
            if (grant.stamp.commit_seq > context.durable_seq) {
                failure = {AYNI_BUSY, AYNI_REASON_GRANT_NOT_DURABLE};
                return false;
            }
            if (grant.parent_grant_id == 0U && grant.depth != 0U) {
                return false;
            }
            chain.indices[chain.count++] = at_index;
            return true;
        });
    if (walked != Walk::root) {
        return failure;
    }
    out = chain;
    return {AYNI_OK, AYNI_REASON_NONE};
}

// C-AUTH-019/022/023: finish shape, scope and parent admission before choosing
// a safe replacement. The completed plan is local and failure leaves out intact.
GrantCheck GrantTable::check_create(const NewGrant &grant, uint32_t expected_parent_revision,
                                    GrantContext context, Admission &out,
                                    const GrantReclaimExclusions &excluded) const {
    if (grant.scope == nullptr || grant.bound_goal_id != 0U) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_GRANT_INPUT};
    }
    const auto input = creation_input(grant, expected_parent_revision, context);
    if (input.status != AYNI_OK) {
        return input;
    }
    uint32_t scope_size = 0;
    const auto normalized =
        normalize_scope(grant.source_domain, *grant.scope, nullptr, 0, scope_size);
    if (normalized != AYNI_BUFFER_TOO_SMALL) {
        return {normalized, AYNI_REASON_GRANT_INPUT};
    }
    Admission admission{
        size(), 0, scope_size, 0, creation_deadline(grant, context.now_us), context.boot_epoch};
    if (grant.parent_grant_id != 0U) {
        uint32_t parent_index = 0;
        if (!find(grant.parent_grant_id, parent_index)) {
            return {AYNI_NOT_FOUND, AYNI_REASON_GRANT_UNKNOWN};
        }
        const Grant &parent = at(parent_index);
        if (expected_parent_revision != parent.revision) {
            return {AYNI_REVISION_CONFLICT, AYNI_REASON_GRANT_REVISION};
        }
        const auto chain = check_chain(parent_index, context, admission.parent_chain);
        if (chain.status != AYNI_OK) {
            return chain;
        }
        const auto contract = check_parent_contract(parent_index, grant);
        if (contract.status != AYNI_OK) {
            return contract;
        }
        if (parent.lifetime == AYNI_GRANT_CURRENT_BOOT &&
            admission.deadline_us > parent.deadline_us) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_PARENT};
        }
        admission.depth = parent.depth + 1U;
        if (admission.depth > limits_.delegation_depth_max) {
            return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GRANT_DEPTH};
        }
        const auto covered = covers(grant.source_domain, scope(parent_index), *grant.scope);
        if (covered.status != AYNI_OK || !covered.covered) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_SCOPE};
        }
    }
    if (scope_size > limits_.scope_max_bytes ||
        grant.source_domain.size > limits_.scope_max_bytes) {
        return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GRANT_CAPACITY};
    }
    BatchPlacement placement{excluded, 0};
    const auto reserved = reserve_placement(context.durable_seq, placement, admission);
    if (reserved.status != AYNI_OK) {
        return reserved;
    }
    out = admission;
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Ordinary creation and a whole Goal approval share append-first placement.
// Safe terminal leaves are ordered by commit then ID (ADR-0030 decision 7).
GrantCheck GrantTable::reserve_placement(uint64_t durable_seq, BatchPlacement &plan,
                                         Admission &admission) const {
    if (plan.appended < limits_.capacity - size()) {
        admission.index = size() + plan.appended;
        admission.replaced_grant_id = 0;
        ++plan.appended;
        return {AYNI_OK, AYNI_REASON_NONE};
    }
    uint32_t earliest = size();
    for (uint32_t index = 0; index < size(); ++index) {
        const Grant &candidate = at(index);
        if (plan.excluded.contains(index) || !reclaimable(index, durable_seq)) {
            continue;
        }
        if (earliest == size() || candidate.stamp.commit_seq < at(earliest).stamp.commit_seq ||
            (candidate.stamp.commit_seq == at(earliest).stamp.commit_seq &&
             candidate.grant_id < at(earliest).grant_id)) {
            earliest = index;
        }
    }
    if (earliest == size()) {
        return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GRANT_CAPACITY};
    }
    admission.index = earliest;
    admission.replaced_grant_id = at(earliest).grant_id;
    plan.excluded.add(earliest);
    return {AYNI_OK, AYNI_REASON_NONE};
}

// Saved work ceilings are already canonical. Validate that representation at
// this boundary without expanding it back into caller-owned typed arrays.
GrantCheck GrantTable::check_goal_create(const NewGrant &grant, ayni_bytes_v1 requested,
                                         GrantContext context, BatchPlacement &plan,
                                         Admission &out) const {
    if (grant.bound_goal_id == 0U || grant.scope != nullptr) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_GRANT_INPUT};
    }
    const auto input = creation_input(grant, 0, context);
    if (input.status != AYNI_OK) {
        return input;
    }
    const auto covered = covers(grant.source_domain, grant.source_domain, requested);
    if (covered.status != AYNI_OK || !covered.covered) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_GRANT_INPUT};
    }
    if (requested.size > limits_.scope_max_bytes ||
        grant.source_domain.size > limits_.scope_max_bytes) {
        return {AYNI_CAPACITY_EXCEEDED, AYNI_REASON_GRANT_CAPACITY};
    }
    Admission admitted{
        size(), 0, requested.size, 0, creation_deadline(grant, context.now_us), context.boot_epoch};
    const auto reserved = reserve_placement(context.durable_seq, plan, admitted);
    if (reserved.status == AYNI_OK) {
        out = admitted;
    }
    return reserved;
}

bool GrantTable::valid_install(const Admission &admission, uint64_t id,
                               foundation::TransitionStamp stamp) const {
    uint32_t ignored = 0;
    return is_kernel_id(id) && id_boot_epoch(id) == admission.boot_epoch && !find(id, ignored) &&
           stamped(stamp) && admission.index <= size() &&
           (admission.index == size()
                ? !grants_.full() && admission.replaced_grant_id == 0U
                : at(admission.index).grant_id == admission.replaced_grant_id);
}

// Consume an unchanged admission plan with a fresh ID/stamp; prepare both
// byte regions before publishing the retained row (ADR-0030 decision 7).
bool GrantTable::insert(const NewGrant &grant, const Admission &admission, uint64_t new_grant_id,
                        foundation::TransitionStamp stamp, uint32_t &out_index) {
    if (!valid_install(admission, new_grant_id, stamp)) {
        return false;
    }
    uint32_t scope_size = 0;
    if (normalize_scope(grant.source_domain, *grant.scope, scope_storage(admission.index),
                        limits_.scope_max_bytes, scope_size) != AYNI_OK) {
        return false;
    }
    publish(grant, admission, new_grant_id, stamp, out_index);
    return true;
}

bool GrantTable::insert_goal(const NewGrant &grant, ayni_bytes_v1 requested,
                             const Admission &admission, uint64_t new_grant_id,
                             foundation::TransitionStamp stamp, uint32_t &out_index) {
    if (!valid_install(admission, new_grant_id, stamp)) {
        return false;
    }
    std::memcpy(scope_storage(admission.index), requested.data, requested.size);
    publish(grant, admission, new_grant_id, stamp, out_index);
    return true;
}

// Both admission paths publish the same immutable fact after preparing Scope.
// The row is visible only after its complete domain and authority are copied.
void GrantTable::publish(const NewGrant &grant, const Admission &admission, uint64_t new_grant_id,
                         foundation::TransitionStamp stamp, uint32_t &out_index) {
    std::memcpy(domain_storage(admission.index), grant.source_domain.data,
                grant.source_domain.size);
    Grant value{new_grant_id,
                grant.subject_instance_id,
                grant.parent_grant_id,
                admission.replaced_grant_id,
                grant.owner_principal_id,
                grant.owner_binding_revision,
                grant.capability_id,
                grant.capability_version,
                grant.lifetime,
                grant.duration_us,
                (grant.flags & AYNI_GRANT_ALLOW_DELEGATION) != 0U,
                admission.depth,
                stamp,
                stamp,
                grant_active_revision,
                AYNI_GRANT_ACTIVE,
                grant.source_domain.size,
                admission.scope_size,
                admission.deadline_us,
                AYNI_REASON_NONE};
    value.bound_goal_id = grant.bound_goal_id;
    if (admission.index == size()) {
        static_cast<void>(grants_.push_back(value));
    } else {
        grants_[admission.index] = value;
    }
    out_index = admission.index;
}

// Schedule finite facts only. An old-boot or recovered deadline is already
// due; otherwise select the earliest deadline and break ties deterministically.
bool GrantTable::next_expiry(GrantContext context, uint32_t &out_index,
                             uint64_t &out_deadline) const {
    bool found = false;
    uint64_t earliest = 0;
    uint32_t selected = 0;
    for (uint32_t index = 0; index < size(); ++index) {
        const Grant &grant = at(index);
        if (grant.lifecycle != AYNI_GRANT_ACTIVE || grant.lifetime != AYNI_GRANT_CURRENT_BOOT) {
            continue;
        }
        const uint64_t deadline = old_boot(grant, context.boot_epoch) ? 0U : grant.deadline_us;
        if (!found || deadline < earliest ||
            (deadline == earliest && grant.grant_id < at(selected).grant_id)) {
            selected = index;
            earliest = deadline;
            found = true;
        }
    }
    if (found) {
        out_index = selected;
        out_deadline = earliest;
    }
    return found;
}

void GrantTable::expire(uint32_t index, foundation::TransitionStamp stamp) {
    Grant &grant = grants_[index];
    if (grant.lifecycle != AYNI_GRANT_ACTIVE || grant.lifetime != AYNI_GRANT_CURRENT_BOOT ||
        !stamped(stamp) || !after(stamp, grant.stamp)) {
        return;
    }
    grant.lifecycle = AYNI_GRANT_EXPIRED;
    grant.revision = grant_terminal_revision;
    grant.stamp = stamp;
}

bool GrantTable::has_old_boot_expiry(uint64_t boot_epoch) const {
    for (uint32_t index = 0; index < size(); ++index) {
        if (at(index).lifecycle == AYNI_GRANT_ACTIVE && old_boot(at(index), boot_epoch)) {
            return true;
        }
    }
    return false;
}

// Startup settles all old finite authority in one transition. Preflight
// every changed row so an invalid stamp cannot partially expire the table.
void GrantTable::expire_old_boot(uint64_t boot_epoch, foundation::TransitionStamp stamp) {
    if (!stamped(stamp)) {
        return;
    }
    for (uint32_t index = 0; index < size(); ++index) {
        if (at(index).lifecycle == AYNI_GRANT_ACTIVE && old_boot(at(index), boot_epoch) &&
            !after(stamp, at(index).stamp)) {
            return;
        }
    }
    for (uint32_t index = 0; index < size(); ++index) {
        if (at(index).lifecycle == AYNI_GRANT_ACTIVE && old_boot(at(index), boot_epoch)) {
            expire(index, stamp);
        }
    }
}

void GrantTable::hold_restored(uint32_t index, ayni_reason_t reason) {
    if (grants_[index].hold_reason == AYNI_REASON_NONE) {
        grants_[index].hold_reason = reason;
    }
}

// C-AUTH-020 re-evaluates only this boot compatibility: never rewrite a saved
// domain or evict an ACTIVE fact just because registration changed temporarily.
void GrantTable::resolve_catalog(const catalog::CapabilityRegistry &catalog) {
    for (uint32_t index = 0; index < size(); ++index) {
        Grant &grant = grants_[index];
        if (catalog_hold(grant.hold_reason)) {
            grant.hold_reason = AYNI_REASON_NONE;
        }
        const auto lookup = catalog.find({grant.capability_id.data(), grant.capability_id.size()},
                                         grant.capability_version);
        if (lookup.kind != catalog::CapabilityRegistry::Lookup::Kind::found) {
            hold_restored(index,
                          lookup.kind == catalog::CapabilityRegistry::Lookup::Kind::unknown_id
                              ? AYNI_REASON_CAPABILITY_UNKNOWN
                              : AYNI_REASON_CAPABILITY_VERSION);
        } else if (!catalog.matches_domain(lookup.index, source_domain(index))) {
            hold_restored(index, AYNI_REASON_GRANT_CATALOG_CHANGED);
        }
    }
}

// The record owns byte shape; this owner validates lifecycle and parent
// relationships before any capacity choice. out_parent is used only for a child.
bool GrantTable::valid_restored_fact(const GrantRecord &record, const RestoredState &state,
                                     uint32_t &out_parent) const {
    if (!valid_grant_record(record) ||
        !lifecycle_state(record, state.created, state.stamp, state.revision, state.lifecycle)) {
        return false;
    }
    uint32_t existing = 0;
    if (find(record.grant_id, existing)) {
        return false;
    }
    const NewGrant &grant = record.grant;
    if (grant.parent_grant_id == 0U) {
        return true;
    }
    uint32_t parent_index = 0;
    if (!find(grant.parent_grant_id, parent_index)) {
        return false;
    }
    const Grant &parent = at(parent_index);
    const auto covered = covers(grant.source_domain, scope(parent_index), record.scope);
    if (check_parent_contract(parent_index, grant).status != AYNI_OK ||
        record.depth != parent.depth + 1U || covered.status != AYNI_OK || !covered.covered ||
        (parent.lifetime == AYNI_GRANT_CURRENT_BOOT &&
         (grant.duration_us > parent.duration_us ||
          id_boot_epoch(record.grant_id) != id_boot_epoch(parent.grant_id)))) {
        return false;
    }
    out_parent = parent_index;
    return true;
}

bool GrantTable::fits_restored_fact(const GrantRecord &record) const {
    return record.depth <= limits_.delegation_depth_max &&
           record.grant.source_domain.size <= limits_.scope_max_bytes &&
           record.scope.size <= limits_.scope_max_bytes;
}

// Journal records name the exact predecessor chosen originally. Replaying
// never reruns eviction order or replaces an unfinished/non-leaf fact (C-AUTH-023).
GrantTable::Restore GrantTable::replay_placement(const GrantRecord &record,
                                                 foundation::TransitionStamp created,
                                                 const GrantReclaimExclusions &excluded,
                                                 uint32_t &out_index) const {
    uint32_t index = size();
    if (record.replaced_grant_id != 0U) {
        if (!find(record.replaced_grant_id, index) || !terminal(at(index)) ||
            !cleanup_finished(at(index)) || has_child(record.replaced_grant_id) ||
            excluded.contains(index) || created.commit_seq <= at(index).reference_release_seq ||
            !after(created, at(index).stamp) ||
            (stamped(at(index).cleanup_stamp) && !after(created, at(index).cleanup_stamp))) {
            return Restore::corrupt;
        }
    } else if (grants_.full()) {
        return Restore::no_room;
    }
    out_index = index;
    return Restore::restored;
}

// Called only after complete admission. Recovered finite facts deliberately
// have no live deadline: neither journal nor snapshot reanchors old authority.
void GrantTable::install_restored(const GrantRecord &record, const RestoredState &state,
                                  uint32_t index) {
    const NewGrant &grant = record.grant;
    std::memcpy(domain_storage(index), grant.source_domain.data, grant.source_domain.size);
    std::memcpy(scope_storage(index), record.scope.data, record.scope.size);
    Grant value{record.grant_id,
                grant.subject_instance_id,
                grant.parent_grant_id,
                record.replaced_grant_id,
                grant.owner_principal_id,
                grant.owner_binding_revision,
                grant.capability_id,
                grant.capability_version,
                grant.lifetime,
                grant.duration_us,
                (grant.flags & AYNI_GRANT_ALLOW_DELEGATION) != 0U,
                record.depth,
                state.created,
                state.stamp,
                state.revision,
                state.lifecycle,
                grant.source_domain.size,
                record.scope.size,
                no_grant_deadline_us,
                AYNI_REASON_NONE};
    value.bound_goal_id = grant.bound_goal_id;
    if (index == size()) {
        static_cast<void>(grants_.push_back(value));
    } else {
        grants_[index] = value;
    }
}

// A journal create observes active ancestors, then consumes its recorded slot.
// All rejection paths precede byte/row publication; classifications stay ordered.
GrantTable::Restore GrantTable::restore_create(const GrantRecord &record,
                                               foundation::TransitionStamp stamp,
                                               const GrantReclaimExclusions &excluded) {
    const RestoredState state{stamp, stamp, grant_active_revision, AYNI_GRANT_ACTIVE};
    uint32_t parent = 0;
    if (!valid_restored_fact(record, state, parent) ||
        (record.grant.parent_grant_id != 0U &&
         history_at(parent, stamp.commit_seq, stamp.transition_id) != AncestorHistory::active)) {
        return Restore::corrupt;
    }
    if (!fits_restored_fact(record)) {
        return Restore::no_room;
    }
    uint32_t index = 0;
    const Restore placement = replay_placement(record, stamp, excluded, index);
    if (placement != Restore::restored) {
        return placement;
    }
    install_restored(record, state, index);
    return Restore::restored;
}

// Replay only a first terminal transition. A revoked descendant must follow
// the next cleanup ID and pair with same-batch root progress (C-AUTH-026).
GrantTable::Restore GrantTable::restore_state(const GrantStateRecord &record,
                                              foundation::TransitionStamp stamp) {
    uint32_t index = 0;
    if (!find(record.grant_id, index) || at(index).lifecycle != AYNI_GRANT_ACTIVE ||
        at(index).revision != grant_active_revision || record.revision != grant_terminal_revision ||
        !stamped(stamp) || !after(stamp, at(index).stamp)) {
        return Restore::corrupt;
    }
    if (record.lifecycle == AYNI_GRANT_EXPIRED) {
        if (at(index).lifetime != AYNI_GRANT_CURRENT_BOOT || record.revoked_by_root_id != 0U) {
            return Restore::corrupt;
        }
        expire(index, stamp);
    } else if (record.lifecycle == AYNI_GRANT_REVOKED) {
        uint32_t root_index = 0;
        if (!find(record.revoked_by_root_id, root_index) || root_index == index ||
            at(root_index).lifecycle != AYNI_GRANT_REVOKED ||
            !is_revocation_root(at(root_index).grant_id, at(root_index).revoked_by_root_id) ||
            cleanup_finished(at(root_index)) || !descendant_of(index, record.revoked_by_root_id) ||
            !after(stamp, at(root_index).cleanup_stamp)) {
            return Restore::corrupt;
        }
        const Grant &root = at(root_index);
        if (record.grant_id <= root.cleanup_after_grant_id ||
            record.grant_id > root.cleanup_through_grant_id) {
            return Restore::corrupt;
        }
        // A state and its progress are atomic in the enclosing batch. A state
        // cannot jump over an unscanned ID, even when that ID is unrelated.
        uint32_t next = 0;
        if (!next_id_after(root.cleanup_after_grant_id, root.cleanup_through_grant_id, next) ||
            at(next).grant_id != record.grant_id) {
            return Restore::corrupt;
        }
        Grant &grant = grants_[index];
        revoke_by(grant, record.revoked_by_root_id, stamp);
    } else if (record.lifecycle == AYNI_GRANT_GOAL_ENDED && at(index).bound_goal_id != 0U &&
               record.revoked_by_root_id == 0U) {
        end_by_goal(grants_[index], stamp);
    } else {
        return Restore::corrupt;
    }
    return Restore::restored;
}

bool GrantTable::valid_revocation_metadata(uint64_t grant_id, uint64_t parent_grant_id,
                                           ayni_grant_lifecycle_t lifecycle,
                                           foundation::TransitionStamp stamp,
                                           uint64_t revoked_by_root_id, uint64_t cleanup_after,
                                           uint64_t cleanup_through,
                                           foundation::TransitionStamp cleanup_stamp) const {
    // A root freezes the retained ID frontier when it is revoked. Every
    // descendant predates that transition, including already terminal ones.
    if (parent_grant_id != 0U) {
        uint32_t parent = 0;
        if (!find(parent_grant_id, parent) ||
            visit_ancestors(parent, limits_.delegation_depth_max, [&](uint32_t index) {
                const Grant &ancestor = at(index);
                return ancestor.lifecycle != AYNI_GRANT_REVOKED ||
                       !is_revocation_root(ancestor.grant_id, ancestor.revoked_by_root_id) ||
                       grant_id <= ancestor.cleanup_through_grant_id;
            }) != Walk::root) {
            return false;
        }
    }
    if (lifecycle != AYNI_GRANT_REVOKED) {
        return revoked_by_root_id == 0U && cleanup_after == 0U && cleanup_through == 0U &&
               cleanup_stamp.transition_id == 0U && cleanup_stamp.commit_seq == 0U;
    }
    if (!is_kernel_id(revoked_by_root_id)) {
        return false;
    }
    if (is_revocation_root(grant_id, revoked_by_root_id)) {
        return is_kernel_id(cleanup_through) && cleanup_through >= grant_id &&
               (cleanup_after == 0U || is_kernel_id(cleanup_after)) &&
               cleanup_after <= cleanup_through && at_or_after(cleanup_stamp, stamp);
    }
    uint32_t root = 0;
    uint32_t parent = 0;
    return cleanup_after == 0U && cleanup_through == 0U && cleanup_stamp.transition_id == 0U &&
           cleanup_stamp.commit_seq == 0U && find(revoked_by_root_id, root) &&
           at(root).lifecycle == AYNI_GRANT_REVOKED &&
           is_revocation_root(at(root).grant_id, at(root).revoked_by_root_id) &&
           grant_id <= at(root).cleanup_after_grant_id &&
           at_or_after(at(root).cleanup_stamp, stamp) && at_or_after(stamp, at(root).stamp) &&
           (parent_grant_id == revoked_by_root_id ||
            (find(parent_grant_id, parent) && descendant_of(parent, revoked_by_root_id)));
}

// Retained snapshots have different placement/history rules from journal
// creates: parent-first append, absent predecessors and possibly later terminal ancestors.
GrantTable::Restore GrantTable::restore_snapshot(const GrantSnapshotRecord &record) {
    if (!valid_revocation_metadata(record.create.grant_id, record.create.grant.parent_grant_id,
                                   record.lifecycle, record.stamp, record.revoked_by_root_id,
                                   record.cleanup_after_grant_id, record.cleanup_through_grant_id,
                                   record.cleanup_stamp)) {
        return Restore::corrupt;
    }
    const RestoredState state{record.created_stamp, record.stamp, record.revision,
                              record.lifecycle};
    uint32_t parent = 0;
    if (!valid_restored_fact(record.create, state, parent) ||
        (record.create.grant.parent_grant_id != 0U &&
         history_at(parent, state.created.commit_seq, state.created.transition_id) ==
             AncestorHistory::invalid)) {
        return Restore::corrupt;
    }
    if (!fits_restored_fact(record.create)) {
        return Restore::no_room;
    }
    uint32_t predecessor = 0;
    if (record.create.replaced_grant_id != 0U &&
        find(record.create.replaced_grant_id, predecessor)) {
        return Restore::corrupt;
    }
    if (grants_.full()) {
        return Restore::no_room;
    }
    // Snapshots contain only retained facts: predecessors are absent and rows
    // append parent before child. Unlike journal replay, no slot is evicted.
    install_restored(record.create, state, size());
    Grant &grant = grants_[size() - 1U]; // snapshot records append parent before child
    grant.revoked_by_root_id = record.revoked_by_root_id;
    grant.cleanup_after_grant_id = record.cleanup_after_grant_id;
    grant.cleanup_through_grant_id = record.cleanup_through_grant_id;
    grant.cleanup_stamp = record.cleanup_stamp;
    return Restore::restored;
}

GrantTable::Restore GrantTable::restore_revocation(const GrantRevocationRecord &record,
                                                   foundation::TransitionStamp stamp) {
    const RevokePlan plan{record.grant_id, record.revision, record.effect,
                          record.cleanup_after_grant_id, record.cleanup_through_grant_id};
    return apply_revoke(plan, stamp) ? Restore::restored : Restore::corrupt;
}

// A progress record must name the next retained ID/final watermark. An active
// descendant cannot be skipped; its same-batch state must already be restored.
GrantTable::Restore GrantTable::restore_progress(const GrantRevocationProgressRecord &record,
                                                 foundation::TransitionStamp stamp) {
    uint32_t root_index = 0;
    if (!find(record.root_grant_id, root_index)) {
        return Restore::corrupt;
    }
    Grant &root = grants_[root_index];
    if (root.lifecycle != AYNI_GRANT_REVOKED ||
        !is_revocation_root(root.grant_id, root.revoked_by_root_id) || cleanup_finished(root) ||
        record.through_grant_id != root.cleanup_through_grant_id ||
        record.after_grant_id <= root.cleanup_after_grant_id ||
        record.after_grant_id > record.through_grant_id || !stamped(stamp) ||
        !after(stamp, root.cleanup_stamp)) {
        return Restore::corrupt;
    }
    uint32_t candidate = 0;
    const bool found =
        next_id_after(root.cleanup_after_grant_id, record.through_grant_id, candidate);
    const uint64_t next = found ? at(candidate).grant_id : record.through_grant_id;
    if (record.after_grant_id != next) {
        return Restore::corrupt;
    }
    if (found && at(candidate).lifecycle == AYNI_GRANT_ACTIVE &&
        descendant_of(candidate, root.grant_id)) {
        return Restore::corrupt; // an ACTIVE descendant needs a same-batch state first
    }
    root.cleanup_after_grant_id = record.after_grant_id;
    root.cleanup_stamp = stamp;
    return Restore::restored;
}

// Before recovery publishes work, prove all retained relationships, including
// roots whose completed cursor may not skip an active descendant (C-AUTH-026).
bool GrantTable::consistent() const {
    for (uint32_t index = 0; index < size(); ++index) {
        const Grant &grant = at(index);
        if (!valid_revocation_metadata(grant.grant_id, grant.parent_grant_id, grant.lifecycle,
                                       grant.stamp, grant.revoked_by_root_id,
                                       grant.cleanup_after_grant_id, grant.cleanup_through_grant_id,
                                       grant.cleanup_stamp)) {
            return false;
        }
        uint32_t predecessor = 0;
        if (grant.replaced_grant_id != 0U && find(grant.replaced_grant_id, predecessor)) {
            return false;
        }
        if (grant.lifecycle == AYNI_GRANT_ACTIVE) {
            for (uint32_t root = 0; root < size(); ++root) {
                const Grant &candidate = at(root);
                if (is_revocation_root(candidate.grant_id, candidate.revoked_by_root_id) &&
                    grant.grant_id <= candidate.cleanup_after_grant_id &&
                    descendant_of(index, candidate.grant_id)) {
                    return false;
                }
            }
        }
        if (grant.parent_grant_id == 0U) {
            if (grant.depth != 0U) {
                return false;
            }
        } else {
            uint32_t parent = 0;
            if (!find(grant.parent_grant_id, parent) || !valid_link(index, parent)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace ayni::authority
