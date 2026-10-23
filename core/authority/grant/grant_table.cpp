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

constexpr uint32_t known_flags =
    AYNI_GRANT_ALLOW_DELEGATION | AYNI_GRANT_REQUIRES_SCENE | AYNI_GRANT_REQUIRES_QUOTA;

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
           (id_boot_epoch(grant.grant_id) != boot_epoch || grant.deadline_us == 0U);
}

bool catalog_hold(ayni_reason_t reason) {
    return reason == AYNI_REASON_CAPABILITY_UNKNOWN || reason == AYNI_REASON_CAPABILITY_VERSION ||
           reason == AYNI_REASON_GRANT_CATALOG_CHANGED;
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
                      (lifecycle == AYNI_GRANT_EXPIRED &&
                       record.grant.lifetime == AYNI_GRANT_CURRENT_BOOT));
}

bool terminal(const Grant &grant) {
    return grant.lifecycle == AYNI_GRANT_EXPIRED || grant.lifecycle == AYNI_GRANT_REVOKED;
}

bool cleanup_finished(const Grant &grant) {
    return grant.revoked_by_root_id != grant.grant_id ||
           grant.cleanup_after_grant_id == grant.cleanup_through_grant_id;
}

bool at_or_after(foundation::TransitionStamp newer, foundation::TransitionStamp older) {
    return same_stamp(newer, older) || after(newer, older);
}

} // namespace

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

uint8_t *GrantTable::domain_storage(uint32_t index) {
    return bytes_ + static_cast<std::size_t>(index) * limits_.scope_max_bytes * 2U;
}

uint8_t *GrantTable::scope_storage(uint32_t index) {
    return domain_storage(index) + limits_.scope_max_bytes;
}

ayni_bytes_v1 GrantTable::source_domain(uint32_t index) const {
    return {bytes_ + static_cast<std::size_t>(index) * limits_.scope_max_bytes * 2U,
            at(index).source_domain_size};
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

bool GrantTable::descendant_of(uint32_t index, uint64_t root_id) const {
    for (uint32_t hops = 0; hops < limits_.delegation_depth_max; ++hops) {
        const uint64_t parent = at(index).parent_grant_id;
        if (parent == root_id) {
            return true;
        }
        if (parent == 0U || !find(parent, index)) {
            return false;
        }
    }
    return false;
}

bool GrantTable::reclaimable(uint32_t index, uint64_t durable_seq) const {
    const Grant &grant = at(index);
    return terminal(grant) && grant.stamp.commit_seq <= durable_seq &&
           grant.cleanup_stamp.commit_seq <= durable_seq && cleanup_finished(grant) &&
           !gated(grant.grant_id) && !has_child(grant.grant_id);
}

bool GrantTable::ancestors_allow_creation(uint32_t index, foundation::TransitionStamp created,
                                          bool snapshot) const {
    for (uint32_t hops = 0; hops <= limits_.delegation_depth_max; ++hops) {
        const Grant &ancestor = at(index);
        if (!after(created, ancestor.created_stamp) ||
            (terminal(ancestor) && (!snapshot || !after(ancestor.stamp, created)))) {
            return false;
        }
        if (ancestor.parent_grant_id == 0U) {
            return true;
        }
        if (!find(ancestor.parent_grant_id, index)) {
            return false;
        }
    }
    return false;
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
        return true; // accepted before reload, but its uncommitted target disappeared
    }
    Grant &grant = grants_[index];
    if (!after(stamp, grant.stamp) ||
        (stamped(grant.cleanup_stamp) && !after(stamp, grant.cleanup_stamp))) {
        return false;
    }
    if (plan.effect == AYNI_INTENT_EFFECT_CHANGED) {
        grant.lifecycle = AYNI_GRANT_REVOKED;
        grant.revision = plan.revision;
        grant.stamp = stamp;
        grant.revoked_by_root_id = grant.grant_id;
        grant.cleanup_after_grant_id = plan.cleanup_after_grant_id;
        grant.cleanup_through_grant_id = plan.cleanup_through_grant_id;
        grant.cleanup_stamp = stamp;
    }
    return true;
}

bool GrantTable::next_revocation_cleanup(CleanupPlan &out) const {
    uint32_t selected = size();
    for (uint32_t index = 0; index < size(); ++index) {
        const Grant &grant = at(index);
        if (grant.lifecycle == AYNI_GRANT_REVOKED && grant.revoked_by_root_id == grant.grant_id &&
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
    for (uint32_t index = 0; index < size(); ++index) {
        const uint64_t id = at(index).grant_id;
        if (id > root.cleanup_after_grant_id && id <= root.cleanup_through_grant_id &&
            (candidate == size() || id < at(candidate).grant_id)) {
            candidate = index;
        }
    }
    const bool revoke = candidate != size() && at(candidate).lifecycle == AYNI_GRANT_ACTIVE &&
                        descendant_of(candidate, root.grant_id);
    out = {root.grant_id,
           candidate == size() ? root.cleanup_through_grant_id : at(candidate).grant_id,
           root.cleanup_through_grant_id, revoke ? at(candidate).grant_id : 0U};
    return true;
}

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
        child.lifecycle = AYNI_GRANT_REVOKED;
        child.revision = grant_terminal_revision;
        child.stamp = stamp;
        child.revoked_by_root_id = root.grant_id;
    }
    root.cleanup_after_grant_id = plan.after_grant_id;
    root.cleanup_stamp = stamp;
    return true;
}

bool GrantTable::same_contract(uint32_t left, const NewGrant &right) const {
    const Grant &parent = at(left);
    return parent.owner_principal_id == right.owner_principal_id &&
           parent.owner_binding_revision == right.owner_binding_revision &&
           parent.capability_version == right.capability_version &&
           parent.capability_id.equals({right.capability_id.data(), right.capability_id.size()}) &&
           same_bytes(source_domain(left), right.source_domain);
}

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
    if (!parent.allow_delegation || child.depth != parent.depth + 1U ||
        child.depth > limits_.delegation_depth_max || !same_contract(parent_index, fact)) {
        return false;
    }
    const auto covered =
        covers(source_domain(parent_index), scope(parent_index), scope(child_index));
    if (covered.status != AYNI_OK || !covered.covered) {
        return false;
    }
    if (parent.lifetime == AYNI_GRANT_CURRENT_BOOT) {
        return child.lifetime == AYNI_GRANT_CURRENT_BOOT &&
               id_boot_epoch(child.grant_id) == id_boot_epoch(parent.grant_id) &&
               child.duration_us <= parent.duration_us &&
               (child.deadline_us == 0U || parent.deadline_us == 0U ||
                child.deadline_us <= parent.deadline_us);
    }
    return true;
}

GrantCheck GrantTable::check_chain(uint32_t index, GrantContext context) const {
    if (index >= size()) {
        return {AYNI_NOT_FOUND, AYNI_REASON_GRANT_UNKNOWN};
    }
    for (uint32_t hops = 0; hops <= limits_.delegation_depth_max; ++hops) {
        const Grant &grant = at(index);
        if (gated(grant.grant_id) || grant.lifecycle == AYNI_GRANT_REVOKED) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_REVOKED};
        }
        if (grant.hold_reason != AYNI_REASON_NONE) {
            return held(grant.hold_reason);
        }
        if (grant.lifecycle == AYNI_GRANT_EXPIRED || old_boot(grant, context.boot_epoch) ||
            (grant.lifetime == AYNI_GRANT_CURRENT_BOOT && context.now_us >= grant.deadline_us)) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_EXPIRED};
        }
        if (grant.stamp.commit_seq > context.durable_seq) {
            return {AYNI_BUSY, AYNI_REASON_GRANT_NOT_DURABLE};
        }
        if (grant.parent_grant_id == 0U) {
            return grant.depth == 0U ? GrantCheck{AYNI_OK, AYNI_REASON_NONE}
                                     : GrantCheck{AYNI_DENIED, AYNI_REASON_GRANT_PARENT};
        }
        uint32_t parent = 0;
        if (!find(grant.parent_grant_id, parent) || !valid_link(index, parent)) {
            return {AYNI_DENIED, AYNI_REASON_GRANT_PARENT};
        }
        index = parent;
    }
    return {AYNI_DENIED, AYNI_REASON_GRANT_PARENT};
}

GrantCheck GrantTable::check_create(const NewGrant &grant, uint32_t expected_parent_revision,
                                    GrantContext context, Admission &out) const {
    const bool permanent = grant.lifetime == AYNI_GRANT_PERMANENT;
    const bool limited = grant.lifetime == AYNI_GRANT_CURRENT_BOOT;
    if (grant.subject_instance_id == 0U || grant.owner_principal_id == 0U ||
        grant.owner_binding_revision == 0U || grant.capability_id.size() == 0U ||
        grant.capability_version == 0U || grant.scope == nullptr ||
        (grant.flags & ~known_flags) != 0U || (!permanent && !limited) ||
        (permanent ? grant.duration_us != 0U : grant.duration_us == 0U) ||
        context.boot_epoch == 0U || context.boot_epoch > foundation::max_boot_epoch ||
        (limited && grant.duration_us > std::numeric_limits<uint64_t>::max() - context.now_us) ||
        (grant.parent_grant_id == 0U ? expected_parent_revision != 0U
                                     : expected_parent_revision == 0U)) {
        return {AYNI_INVALID_ARGUMENT, AYNI_REASON_GRANT_INPUT};
    }
    if ((grant.flags & (AYNI_GRANT_REQUIRES_SCENE | AYNI_GRANT_REQUIRES_QUOTA)) != 0U) {
        return {AYNI_UNSUPPORTED, AYNI_REASON_GRANT_UNSUPPORTED};
    }
    uint32_t scope_size = 0;
    const auto normalized =
        normalize_scope(grant.source_domain, *grant.scope, nullptr, 0, scope_size);
    if (normalized != AYNI_BUFFER_TOO_SMALL) {
        return {normalized, AYNI_REASON_GRANT_INPUT};
    }
    Admission admission{size(),
                        0,
                        scope_size,
                        0,
                        limited ? context.now_us + grant.duration_us : 0U,
                        context.boot_epoch};
    if (grant.parent_grant_id != 0U) {
        uint32_t parent_index = 0;
        if (!find(grant.parent_grant_id, parent_index)) {
            return {AYNI_NOT_FOUND, AYNI_REASON_GRANT_UNKNOWN};
        }
        const Grant &parent = at(parent_index);
        if (expected_parent_revision != parent.revision) {
            return {AYNI_REVISION_CONFLICT, AYNI_REASON_GRANT_REVISION};
        }
        const auto chain = check_chain(parent_index, context);
        if (chain.status != AYNI_OK) {
            return chain;
        }
        if (!parent.allow_delegation || !same_contract(parent_index, grant) ||
            (parent.lifetime == AYNI_GRANT_CURRENT_BOOT &&
             (!limited || admission.deadline_us > parent.deadline_us))) {
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
    if (grants_.full()) {
        uint32_t earliest = size();
        for (uint32_t index = 0; index < size(); ++index) {
            const Grant &candidate = at(index);
            if (!reclaimable(index, context.durable_seq)) {
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
    }
    out = admission;
    return {AYNI_OK, AYNI_REASON_NONE};
}

bool GrantTable::insert(const NewGrant &grant, const Admission &admission, uint64_t new_grant_id,
                        foundation::TransitionStamp stamp, uint32_t &out_index) {
    uint32_t ignored = 0;
    if (!is_kernel_id(new_grant_id) || id_boot_epoch(new_grant_id) != admission.boot_epoch ||
        find(new_grant_id, ignored) || !stamped(stamp) || admission.index > size() ||
        (admission.index == size() ? grants_.full() || admission.replaced_grant_id != 0U
                                   : at(admission.index).grant_id != admission.replaced_grant_id)) {
        return false;
    }
    uint32_t scope_size = 0;
    if (normalize_scope(grant.source_domain, *grant.scope, scope_storage(admission.index),
                        limits_.scope_max_bytes, scope_size) != AYNI_OK) {
        return false;
    }
    std::memcpy(domain_storage(admission.index), grant.source_domain.data,
                grant.source_domain.size);
    const Grant value{new_grant_id,
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
                      scope_size,
                      admission.deadline_us,
                      AYNI_REASON_NONE};
    if (admission.index == size()) {
        static_cast<void>(grants_.push_back(value));
    } else {
        grants_[admission.index] = value;
    }
    out_index = admission.index;
    return true;
}

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
        } else if (!same_bytes(source_domain(index), catalog.domain_view(lookup.index))) {
            hold_restored(index, AYNI_REASON_GRANT_CATALOG_CHANGED);
        }
    }
}

GrantTable::Restore GrantTable::restore(const GrantRecord &record,
                                        foundation::TransitionStamp created,
                                        foundation::TransitionStamp stamp, uint32_t revision,
                                        ayni_grant_lifecycle_t lifecycle, bool snapshot) {
    if (!valid_grant_record(record) ||
        !lifecycle_state(record, created, stamp, revision, lifecycle)) {
        return Restore::corrupt;
    }
    uint32_t existing = 0;
    if (find(record.grant_id, existing)) {
        return Restore::corrupt;
    }
    const NewGrant &grant = record.grant;
    if (grant.parent_grant_id != 0U) {
        uint32_t parent_index = 0;
        if (!find(grant.parent_grant_id, parent_index)) {
            return Restore::corrupt;
        }
        const Grant &parent = at(parent_index);
        const auto covered = covers(grant.source_domain, scope(parent_index), record.scope);
        if (!parent.allow_delegation || !same_contract(parent_index, grant) ||
            record.depth != parent.depth + 1U || covered.status != AYNI_OK || !covered.covered ||
            !ancestors_allow_creation(parent_index, created, snapshot) ||
            (parent.lifetime == AYNI_GRANT_CURRENT_BOOT &&
             (grant.lifetime != AYNI_GRANT_CURRENT_BOOT || grant.duration_us > parent.duration_us ||
              id_boot_epoch(record.grant_id) != id_boot_epoch(parent.grant_id)))) {
            return Restore::corrupt;
        }
    }
    if (record.depth > limits_.delegation_depth_max ||
        grant.source_domain.size > limits_.scope_max_bytes ||
        record.scope.size > limits_.scope_max_bytes) {
        return Restore::no_room;
    }
    uint32_t index = size();
    if (!snapshot && record.replaced_grant_id != 0U) {
        if (!find(record.replaced_grant_id, index) || !terminal(at(index)) ||
            !cleanup_finished(at(index)) || has_child(record.replaced_grant_id) ||
            !after(created, at(index).stamp) ||
            (stamped(at(index).cleanup_stamp) && !after(created, at(index).cleanup_stamp))) {
            return Restore::corrupt;
        }
    } else {
        if (snapshot && record.replaced_grant_id != 0U &&
            find(record.replaced_grant_id, existing)) {
            return Restore::corrupt;
        }
        if (grants_.full()) {
            return Restore::no_room;
        }
    }
    std::memcpy(domain_storage(index), grant.source_domain.data, grant.source_domain.size);
    std::memcpy(scope_storage(index), record.scope.data, record.scope.size);
    const Grant value{record.grant_id,
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
                      created,
                      stamp,
                      revision,
                      lifecycle,
                      grant.source_domain.size,
                      record.scope.size,
                      0,
                      AYNI_REASON_NONE};
    if (index == size()) {
        static_cast<void>(grants_.push_back(value));
    } else {
        grants_[index] = value;
    }
    return Restore::restored;
}

GrantTable::Restore GrantTable::restore_create(const GrantRecord &record,
                                               foundation::TransitionStamp stamp) {
    return restore(record, stamp, stamp, grant_active_revision, AYNI_GRANT_ACTIVE, false);
}

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
            at(root_index).revoked_by_root_id != record.revoked_by_root_id ||
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
        for (uint32_t row = 0; row < size(); ++row) {
            const uint64_t id = at(row).grant_id;
            if (id > root.cleanup_after_grant_id && id < record.grant_id) {
                return Restore::corrupt;
            }
        }
        Grant &grant = grants_[index];
        grant.lifecycle = AYNI_GRANT_REVOKED;
        grant.revision = grant_terminal_revision;
        grant.stamp = stamp;
        grant.revoked_by_root_id = record.revoked_by_root_id;
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
    uint64_t ancestor_id = parent_grant_id;
    for (uint32_t hops = 0; ancestor_id != 0U && hops < limits_.delegation_depth_max; ++hops) {
        uint32_t ancestor_index = 0;
        if (!find(ancestor_id, ancestor_index)) {
            return false;
        }
        const Grant &ancestor = at(ancestor_index);
        if (ancestor.lifecycle == AYNI_GRANT_REVOKED &&
            ancestor.revoked_by_root_id == ancestor_id &&
            grant_id > ancestor.cleanup_through_grant_id) {
            return false;
        }
        ancestor_id = ancestor.parent_grant_id;
    }
    if (ancestor_id != 0U) {
        return false;
    }
    if (lifecycle != AYNI_GRANT_REVOKED) {
        return revoked_by_root_id == 0U && cleanup_after == 0U && cleanup_through == 0U &&
               cleanup_stamp.transition_id == 0U && cleanup_stamp.commit_seq == 0U;
    }
    if (!is_kernel_id(revoked_by_root_id)) {
        return false;
    }
    if (revoked_by_root_id == grant_id) {
        return is_kernel_id(cleanup_through) && cleanup_through >= grant_id &&
               (cleanup_after == 0U || is_kernel_id(cleanup_after)) &&
               cleanup_after <= cleanup_through && at_or_after(cleanup_stamp, stamp);
    }
    uint32_t root = 0;
    uint32_t parent = 0;
    return cleanup_after == 0U && cleanup_through == 0U && cleanup_stamp.transition_id == 0U &&
           cleanup_stamp.commit_seq == 0U && find(revoked_by_root_id, root) &&
           at(root).lifecycle == AYNI_GRANT_REVOKED &&
           at(root).revoked_by_root_id == revoked_by_root_id &&
           grant_id <= at(root).cleanup_after_grant_id &&
           at_or_after(at(root).cleanup_stamp, stamp) && at_or_after(stamp, at(root).stamp) &&
           (parent_grant_id == revoked_by_root_id ||
            (find(parent_grant_id, parent) && descendant_of(parent, revoked_by_root_id)));
}

GrantTable::Restore GrantTable::restore_snapshot(const GrantSnapshotRecord &record) {
    if (!valid_revocation_metadata(record.create.grant_id, record.create.grant.parent_grant_id,
                                   record.lifecycle, record.stamp, record.revoked_by_root_id,
                                   record.cleanup_after_grant_id, record.cleanup_through_grant_id,
                                   record.cleanup_stamp)) {
        return Restore::corrupt;
    }
    const Restore result = restore(record.create, record.created_stamp, record.stamp,
                                   record.revision, record.lifecycle, true);
    if (result != Restore::restored) {
        return result;
    }
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

GrantTable::Restore GrantTable::restore_progress(const GrantRevocationProgressRecord &record,
                                                 foundation::TransitionStamp stamp) {
    uint32_t root_index = 0;
    if (!find(record.root_grant_id, root_index)) {
        return Restore::corrupt;
    }
    Grant &root = grants_[root_index];
    if (root.lifecycle != AYNI_GRANT_REVOKED || root.revoked_by_root_id != root.grant_id ||
        cleanup_finished(root) || record.through_grant_id != root.cleanup_through_grant_id ||
        record.after_grant_id <= root.cleanup_after_grant_id ||
        record.after_grant_id > record.through_grant_id || !stamped(stamp) ||
        !after(stamp, root.cleanup_stamp)) {
        return Restore::corrupt;
    }
    uint64_t next = record.through_grant_id;
    for (uint32_t index = 0; index < size(); ++index) {
        const Grant &candidate = at(index);
        if (candidate.grant_id > root.cleanup_after_grant_id && candidate.grant_id < next) {
            next = candidate.grant_id;
        }
    }
    if (record.after_grant_id != next) {
        return Restore::corrupt;
    }
    uint32_t candidate = 0;
    if (find(next, candidate) && at(candidate).lifecycle == AYNI_GRANT_ACTIVE &&
        descendant_of(candidate, root.grant_id)) {
        return Restore::corrupt; // an ACTIVE descendant needs a same-batch state first
    }
    root.cleanup_after_grant_id = record.after_grant_id;
    root.cleanup_stamp = stamp;
    return Restore::restored;
}

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
                if (candidate.revoked_by_root_id == candidate.grant_id &&
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
