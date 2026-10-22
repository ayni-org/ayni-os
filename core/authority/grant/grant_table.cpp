#include "authority/grant/grant_table.h"

#include "authority/grant/grant_records.h"
#include "authority/scope/scope.h"
#include "foundation/kernel_id.h"

#include <cstring>
#include <limits>

namespace ayni::authority {
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

bool known_id(uint64_t id) {
    return (id >> 32U) != 0U && static_cast<uint32_t>(id) != 0U;
}

bool old_boot(const Grant &grant, uint64_t boot_epoch) {
    return grant.lifetime == AYNI_GRANT_CURRENT_BOOT &&
           ((grant.grant_id >> 32U) != boot_epoch || grant.deadline_us == 0U);
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
               ? revision == 1U && same_stamp(created, stamp)
               : lifecycle == AYNI_GRANT_EXPIRED && revision == 2U &&
                     record.grant.lifetime == AYNI_GRANT_CURRENT_BOOT && after(stamp, created);
}

} // namespace

static_assert(GrantTable::storage_size({AYNI_GRANT_CAPACITY_MAX, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX,
                                        AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX}) <=
                  std::numeric_limits<uint32_t>::max(),
              "configured Grant arenas fit on a 32-bit Host");

GrantTable::GrantTable(void *storage, Limits limits)
    : grants_(storage, limits.capacity),
      bytes_(limits.capacity == 0U
                 ? nullptr
                 : static_cast<uint8_t *>(storage) +
                       foundation::BoundedArray<Grant>::storage_size(limits.capacity)),
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
               (child.grant_id >> 32U) == (parent.grant_id >> 32U) &&
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
            if (candidate.lifecycle != AYNI_GRANT_EXPIRED ||
                candidate.stamp.commit_seq > context.durable_seq || has_child(candidate.grant_id)) {
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
    if (!known_id(new_grant_id) || (new_grant_id >> 32U) != admission.boot_epoch ||
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
                      1,
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
    grant.revision = 2;
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
            !after(created, parent.created_stamp) ||
            (parent.lifecycle == AYNI_GRANT_EXPIRED &&
             (!snapshot || !after(parent.stamp, created))) ||
            (parent.lifetime == AYNI_GRANT_CURRENT_BOOT &&
             (grant.lifetime != AYNI_GRANT_CURRENT_BOOT || grant.duration_us > parent.duration_us ||
              (record.grant_id >> 32U) != (parent.grant_id >> 32U)))) {
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
        if (!find(record.replaced_grant_id, index) || at(index).lifecycle != AYNI_GRANT_EXPIRED ||
            has_child(record.replaced_grant_id) || !after(created, at(index).stamp)) {
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
    return restore(record, stamp, stamp, 1, AYNI_GRANT_ACTIVE, false);
}

GrantTable::Restore GrantTable::restore_state(const GrantStateRecord &record,
                                              foundation::TransitionStamp stamp) {
    uint32_t index = 0;
    if (!find(record.grant_id, index) || at(index).lifecycle != AYNI_GRANT_ACTIVE ||
        at(index).lifetime != AYNI_GRANT_CURRENT_BOOT || at(index).revision != 1U ||
        record.revision != 2U || record.lifecycle != AYNI_GRANT_EXPIRED || !stamped(stamp) ||
        !after(stamp, at(index).stamp)) {
        return Restore::corrupt;
    }
    expire(index, stamp);
    return Restore::restored;
}

GrantTable::Restore GrantTable::restore_snapshot(const GrantSnapshotRecord &record) {
    return restore(record.create, record.created_stamp, record.stamp, record.revision,
                   record.lifecycle, true);
}

bool GrantTable::consistent() const {
    for (uint32_t index = 0; index < size(); ++index) {
        const Grant &grant = at(index);
        uint32_t predecessor = 0;
        if (grant.replaced_grant_id != 0U && find(grant.replaced_grant_id, predecessor)) {
            return false;
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
