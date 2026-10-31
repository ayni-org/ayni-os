#include "authority/grant/grant_table.h"
#include "foundation/workspace.h"

#include "authority/grant/grant_restore.h"
#include "authority/grant/grant_state.h"
#include "authority/scope/scope.h"
#include "foundation/kernel_id.h"

#include <cstring>
#include <limits>

namespace ayni::authority {

using foundation::id_boot_epoch;
using foundation::is_kernel_id;
using namespace grant_detail;

namespace {

// Reject malformed scalars before unsupported features and all scope/table
// work. Finite deadline addition is bounded before creating an Admission.
GrantCheck creation_input(const NewGrant &grant, uint32_t expected_parent_revision,
                          GrantContext context) {
    const bool limited = grant.lifetime == AYNI_GRANT_CURRENT_BOOT;
    if (grant.subject_instance_id == 0U || grant.owner_principal_id == 0U ||
        grant.owner_binding_revision == 0U || grant.capability_id.size() == 0U ||
        grant.capability_version == 0U ||
        grant.business_contract.size > AYNI_BUSINESS_CONTRACT_BYTES_MAX ||
        (grant.business_contract.size != 0U && grant.business_contract.data == nullptr) ||
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
                       RevocationGate::storage_size(limits.capacity)),
      limits_(limits) {}

uint8_t *GrantTable::domain_storage(uint32_t index) const {
    return bytes_ + static_cast<std::size_t>(index) *
                        (static_cast<std::size_t>(limits_.scope_max_bytes) * grant_value_regions +
                         limits_.business_bytes);
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

uint8_t *GrantTable::business_storage(uint32_t index) const {
    return domain_storage(index) +
           static_cast<std::size_t>(limits_.scope_max_bytes) * grant_value_regions;
}
ayni_bytes_v1 GrantTable::business_contract(uint32_t index) const {
    return {business_storage(index), at(index).business_contract_size};
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
                                    ayni_bytes_v1 requested_scope,
                                    ayni_bytes_v1 original_business) const {
    const auto &grant = at(index);
    if (!grant.capability_id.equals({capability.data(), capability.size()}) ||
        grant.capability_version != version || !same_bytes(source_domain(index), original_domain) ||
        !same_bytes(business_contract(index), original_business)) {
        return false;
    }
    const auto covered = covers(original_domain, scope(index), requested_scope);
    return covered.status == AYNI_OK && covered.covered;
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
        same_bytes(business_contract(parent_index), child.business_contract) &&
        (parent.lifetime != AYNI_GRANT_CURRENT_BOOT || child.lifetime == AYNI_GRANT_CURRENT_BOOT);
    return compatible ? GrantCheck{AYNI_OK, AYNI_REASON_NONE}
                      : GrantCheck{AYNI_DENIED, AYNI_REASON_GRANT_PARENT};
}

// Validate a retained parent edge using stored duration/boot evidence and,
// when available, live deadlines. Recovery deliberately has no live deadlines.
bool GrantTable::valid_link(uint32_t child_index, uint32_t parent_index) const {
    const Grant &child = at(child_index);
    const Grant &parent = at(parent_index);
    NewGrant fact{child.subject_instance_id,
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
    fact.business_contract = business_contract(child_index);
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
        grant.source_domain.size > limits_.scope_max_bytes ||
        grant.business_contract.size > limits_.business_bytes) {
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
        grant.source_domain.size > limits_.scope_max_bytes ||
        grant.business_contract.size > limits_.business_bytes) {
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
    if (grant.business_contract.size != 0U) {
        std::memcpy(business_storage(admission.index), grant.business_contract.data,
                    grant.business_contract.size);
    }
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
    value.business_contract_size = grant.business_contract.size;
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
        } else if (!catalog.matches_domain(lookup.index, source_domain(index)) ||
                   !catalog.matches_business(lookup.index, business_contract(index))) {
            hold_restored(index, AYNI_REASON_GRANT_CATALOG_CHANGED);
        }
    }
}

// Public table operations delegate historical proof to its restoration facet.
bool GrantTable::active_at(uint32_t index, uint64_t commit_seq) const {
    return GrantRestore::active_at(*this, index, commit_seq);
}
bool GrantTable::consistent() const {
    return GrantRestore::consistent(*this);
}
GrantTable::Restore GrantTable::restore_create(ayni::foundation::Workspace &workspace,
                                               const GrantRecord &record,
                                               foundation::TransitionStamp stamp,
                                               const GrantReclaimExclusions &excluded) {
    return GrantRestore::restore_create(workspace, *this, record, stamp, excluded);
}
GrantTable::Restore GrantTable::restore_state(const GrantStateRecord &record,
                                              foundation::TransitionStamp stamp) {
    return GrantRestore::restore_state(*this, record, stamp);
}
GrantTable::Restore GrantTable::restore_snapshot(ayni::foundation::Workspace &workspace,
                                                 const GrantSnapshotRecord &record) {
    return GrantRestore::restore_snapshot(workspace, *this, record);
}
GrantTable::Restore GrantTable::restore_revocation(const GrantRevocationRecord &record,
                                                   foundation::TransitionStamp stamp) {
    return GrantRestore::restore_revocation(*this, record, stamp);
}
GrantTable::Restore GrantTable::restore_progress(const GrantRevocationProgressRecord &record,
                                                 foundation::TransitionStamp stamp) {
    return GrantRestore::restore_progress(*this, record, stamp);
}

} // namespace ayni::authority
