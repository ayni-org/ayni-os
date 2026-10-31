// Historical validation and publication of Grant facts; never restores volatile Gates.
// All record/history/capacity checks precede row publication (ADR-0030/0031).
#include "authority/grant/grant_restore.h"
#include "authority/grant/grant_records.h"
#include "authority/grant/grant_state.h"
#include "authority/scope/scope.h"
#include "foundation/kernel_id.h"
#include "foundation/workspace.h"
#include <cstring>
namespace ayni::authority {
using foundation::id_boot_epoch;
using foundation::is_kernel_id;
using namespace grant_detail;
namespace {
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
} // namespace
// Classify the same history once: snapshots may show later terminal facts;
// replayed creates must precede every ancestor's terminal transition (ADR-0030).
GrantRestore::AncestorHistory GrantRestore::history_at(const GrantTable &table, uint32_t index,
                                                       uint64_t commit_seq,
                                                       std::optional<uint64_t> transition_id) {
    auto history = AncestorHistory::active;
    const auto walked = table.visit_ancestors(
        index, table.limits_.delegation_depth_max + 1U, [&](uint32_t ancestor_index) {
            const Grant &ancestor = table.at(ancestor_index);
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
    return walked == GrantTable::Walk::root ? history : AncestorHistory::invalid;
}

bool GrantRestore::active_at(const GrantTable &table, uint32_t index, uint64_t commit_seq) {
    return history_at(table, index, commit_seq) != AncestorHistory::invalid;
}

// The record owns byte shape; this owner validates lifecycle and parent
// relationships before any capacity choice. out_parent is used only for a child.
bool GrantRestore::valid_restored_fact(ayni::foundation::Workspace &workspace,
                                       const GrantTable &table, const GrantRecord &record,
                                       const RestoredState &state, uint32_t &out_parent) {
    if (!valid_grant_record(workspace, record) ||
        !lifecycle_state(record, state.created, state.stamp, state.revision, state.lifecycle)) {
        return false;
    }
    uint32_t existing = 0;
    if (table.find(record.grant_id, existing)) {
        return false;
    }
    const NewGrant &grant = record.grant;
    if (grant.parent_grant_id == 0U) {
        return true;
    }
    uint32_t parent_index = 0;
    if (!table.find(grant.parent_grant_id, parent_index)) {
        return false;
    }
    const Grant &parent = table.at(parent_index);
    const auto covered = covers(grant.source_domain, table.scope(parent_index), record.scope);
    if (table.check_parent_contract(parent_index, grant).status != AYNI_OK ||
        record.depth != parent.depth + 1U || covered.status != AYNI_OK || !covered.covered ||
        (parent.lifetime == AYNI_GRANT_CURRENT_BOOT &&
         (grant.duration_us > parent.duration_us ||
          id_boot_epoch(record.grant_id) != id_boot_epoch(parent.grant_id)))) {
        return false;
    }
    out_parent = parent_index;
    return true;
}

bool GrantRestore::fits_restored_fact(const GrantTable &table, const GrantRecord &record) {
    return record.depth <= table.limits_.delegation_depth_max &&
           record.grant.source_domain.size <= table.limits_.scope_max_bytes &&
           record.grant.business_contract.size <= table.limits_.business_bytes &&
           record.scope.size <= table.limits_.scope_max_bytes;
}

// Journal records name the exact predecessor chosen originally. Replaying
// never reruns eviction order or replaces an unfinished/non-leaf fact (C-AUTH-023).
GrantTable::Restore GrantRestore::replay_placement(const GrantTable &table,
                                                   const GrantRecord &record,
                                                   foundation::TransitionStamp created,
                                                   const GrantReclaimExclusions &excluded,
                                                   uint32_t &out_index) {
    uint32_t index = table.size();
    if (record.replaced_grant_id != 0U) {
        if (!table.find(record.replaced_grant_id, index) || !terminal(table.at(index)) ||
            !cleanup_finished(table.at(index)) || table.has_child(record.replaced_grant_id) ||
            excluded.contains(index) ||
            created.commit_seq <= table.at(index).reference_release_seq ||
            !after(created, table.at(index).stamp) ||
            (stamped(table.at(index).cleanup_stamp) &&
             !after(created, table.at(index).cleanup_stamp))) {
            return Restore::corrupt;
        }
    } else if (table.grants_.full()) {
        return Restore::no_room;
    }
    out_index = index;
    return Restore::restored;
}

// Called only after complete admission. Recovered finite facts deliberately
// have no live deadline: neither journal nor snapshot reanchors old authority.
void GrantRestore::install_restored(GrantTable &table, const GrantRecord &record,
                                    const RestoredState &state, uint32_t index) {
    const NewGrant &grant = record.grant;
    std::memcpy(table.domain_storage(index), grant.source_domain.data, grant.source_domain.size);
    std::memcpy(table.scope_storage(index), record.scope.data, record.scope.size);
    if (grant.business_contract.size != 0U) {
        std::memcpy(table.business_storage(index), grant.business_contract.data,
                    grant.business_contract.size);
    }
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
    value.business_contract_size = grant.business_contract.size;
    if (index == table.size()) {
        static_cast<void>(table.grants_.push_back(value));
    } else {
        table.grants_[index] = value;
    }
}

// A journal create observes active ancestors, then consumes its recorded slot.
// All rejection paths precede byte/row publication; classifications stay ordered.
GrantTable::Restore GrantRestore::restore_create(ayni::foundation::Workspace &workspace,
                                                 GrantTable &table, const GrantRecord &record,
                                                 foundation::TransitionStamp stamp,
                                                 const GrantReclaimExclusions &excluded) {
    const RestoredState state{stamp, stamp, grant_active_revision, AYNI_GRANT_ACTIVE};
    uint32_t parent = 0;
    if (!valid_restored_fact(workspace, table, record, state, parent) ||
        (record.grant.parent_grant_id != 0U &&
         history_at(table, parent, stamp.commit_seq, stamp.transition_id) !=
             AncestorHistory::active)) {
        return Restore::corrupt;
    }
    if (!fits_restored_fact(table, record)) {
        return Restore::no_room;
    }
    uint32_t index = 0;
    const Restore placement = replay_placement(table, record, stamp, excluded, index);
    if (placement != Restore::restored) {
        return placement;
    }
    install_restored(table, record, state, index);
    return Restore::restored;
}

// Replay only a first terminal transition. A revoked descendant must follow
// the next cleanup ID and pair with same-batch root progress (C-AUTH-026).
GrantTable::Restore GrantRestore::restore_state(GrantTable &table, const GrantStateRecord &record,
                                                foundation::TransitionStamp stamp) {
    uint32_t index = 0;
    if (!table.find(record.grant_id, index) || table.at(index).lifecycle != AYNI_GRANT_ACTIVE ||
        table.at(index).revision != grant_active_revision ||
        record.revision != grant_terminal_revision || !stamped(stamp) ||
        !after(stamp, table.at(index).stamp)) {
        return Restore::corrupt;
    }
    if (record.lifecycle == AYNI_GRANT_EXPIRED) {
        if (table.at(index).lifetime != AYNI_GRANT_CURRENT_BOOT ||
            record.revoked_by_root_id != 0U) {
            return Restore::corrupt;
        }
        table.expire(index, stamp);
    } else if (record.lifecycle == AYNI_GRANT_REVOKED) {
        uint32_t root_index = 0;
        if (!table.find(record.revoked_by_root_id, root_index) || root_index == index ||
            table.at(root_index).lifecycle != AYNI_GRANT_REVOKED ||
            !is_revocation_root(table.at(root_index).grant_id,
                                table.at(root_index).revoked_by_root_id) ||
            cleanup_finished(table.at(root_index)) ||
            !table.descendant_of(index, record.revoked_by_root_id) ||
            !after(stamp, table.at(root_index).cleanup_stamp)) {
            return Restore::corrupt;
        }
        const Grant &root = table.at(root_index);
        if (record.grant_id <= root.cleanup_after_grant_id ||
            record.grant_id > root.cleanup_through_grant_id) {
            return Restore::corrupt;
        }
        // A state and its progress are atomic in the enclosing batch. A state
        // cannot jump over an unscanned ID, even when that ID is unrelated.
        uint32_t next = 0;
        if (!table.next_id_after(root.cleanup_after_grant_id, root.cleanup_through_grant_id,
                                 next) ||
            table.at(next).grant_id != record.grant_id) {
            return Restore::corrupt;
        }
        Grant &grant = table.grants_[index];
        revoke_by(grant, record.revoked_by_root_id, stamp);
    } else if (record.lifecycle == AYNI_GRANT_GOAL_ENDED && table.at(index).bound_goal_id != 0U &&
               record.revoked_by_root_id == 0U) {
        end_by_goal(table.grants_[index], stamp);
    } else {
        return Restore::corrupt;
    }
    return Restore::restored;
}

bool GrantRestore::valid_revocation_metadata(const GrantTable &table, uint64_t grant_id,
                                             uint64_t parent_grant_id,
                                             ayni_grant_lifecycle_t lifecycle,
                                             foundation::TransitionStamp stamp,
                                             uint64_t revoked_by_root_id, uint64_t cleanup_after,
                                             uint64_t cleanup_through,
                                             foundation::TransitionStamp cleanup_stamp) {
    // A root freezes the retained ID frontier when it is revoked. Every
    // descendant predates that transition, including already terminal ones.
    if (parent_grant_id != 0U) {
        uint32_t parent = 0;
        if (!table.find(parent_grant_id, parent) ||
            table.visit_ancestors(parent, table.limits_.delegation_depth_max, [&](uint32_t index) {
                const Grant &ancestor = table.at(index);
                return ancestor.lifecycle != AYNI_GRANT_REVOKED ||
                       !is_revocation_root(ancestor.grant_id, ancestor.revoked_by_root_id) ||
                       grant_id <= ancestor.cleanup_through_grant_id;
            }) != GrantTable::Walk::root) {
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
           cleanup_stamp.commit_seq == 0U && table.find(revoked_by_root_id, root) &&
           table.at(root).lifecycle == AYNI_GRANT_REVOKED &&
           is_revocation_root(table.at(root).grant_id, table.at(root).revoked_by_root_id) &&
           grant_id <= table.at(root).cleanup_after_grant_id &&
           at_or_after(table.at(root).cleanup_stamp, stamp) &&
           at_or_after(stamp, table.at(root).stamp) &&
           (parent_grant_id == revoked_by_root_id ||
            (table.find(parent_grant_id, parent) &&
             table.descendant_of(parent, revoked_by_root_id)));
}

// Retained snapshots have different placement/history rules from journal
// creates: parent-first append, absent predecessors and possibly later terminal ancestors.
GrantTable::Restore GrantRestore::restore_snapshot(ayni::foundation::Workspace &workspace,
                                                   GrantTable &table,
                                                   const GrantSnapshotRecord &record) {
    if (!valid_revocation_metadata(
            table, record.create.grant_id, record.create.grant.parent_grant_id, record.lifecycle,
            record.stamp, record.revoked_by_root_id, record.cleanup_after_grant_id,
            record.cleanup_through_grant_id, record.cleanup_stamp)) {
        return Restore::corrupt;
    }
    const RestoredState state{record.created_stamp, record.stamp, record.revision,
                              record.lifecycle};
    uint32_t parent = 0;
    if (!valid_restored_fact(workspace, table, record.create, state, parent) ||
        (record.create.grant.parent_grant_id != 0U &&
         history_at(table, parent, state.created.commit_seq, state.created.transition_id) ==
             AncestorHistory::invalid)) {
        return Restore::corrupt;
    }
    if (!fits_restored_fact(table, record.create)) {
        return Restore::no_room;
    }
    uint32_t predecessor = 0;
    if (record.create.replaced_grant_id != 0U &&
        table.find(record.create.replaced_grant_id, predecessor)) {
        return Restore::corrupt;
    }
    if (table.grants_.full()) {
        return Restore::no_room;
    }
    // Snapshots contain only retained facts: predecessors are absent and rows
    // append parent before child. Unlike journal replay, no slot is evicted.
    install_restored(table, record.create, state, table.size());
    Grant &grant = table.grants_[table.size() - 1U]; // snapshot records append parent before child
    grant.revoked_by_root_id = record.revoked_by_root_id;
    grant.cleanup_after_grant_id = record.cleanup_after_grant_id;
    grant.cleanup_through_grant_id = record.cleanup_through_grant_id;
    grant.cleanup_stamp = record.cleanup_stamp;
    return Restore::restored;
}

GrantTable::Restore GrantRestore::restore_revocation(GrantTable &table,
                                                     const GrantRevocationRecord &record,
                                                     foundation::TransitionStamp stamp) {
    const GrantTable::RevokePlan plan{record.grant_id, record.revision, record.effect,
                                      record.cleanup_after_grant_id,
                                      record.cleanup_through_grant_id};
    return table.apply_revoke(plan, stamp) ? Restore::restored : Restore::corrupt;
}

// A progress record must name the next retained ID/final watermark. An active
// descendant cannot be skipped; its same-batch state must already be restored.
GrantTable::Restore GrantRestore::restore_progress(GrantTable &table,
                                                   const GrantRevocationProgressRecord &record,
                                                   foundation::TransitionStamp stamp) {
    uint32_t root_index = 0;
    if (!table.find(record.root_grant_id, root_index)) {
        return Restore::corrupt;
    }
    Grant &root = table.grants_[root_index];
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
        table.next_id_after(root.cleanup_after_grant_id, record.through_grant_id, candidate);
    const uint64_t next = found ? table.at(candidate).grant_id : record.through_grant_id;
    if (record.after_grant_id != next) {
        return Restore::corrupt;
    }
    if (found && table.at(candidate).lifecycle == AYNI_GRANT_ACTIVE &&
        table.descendant_of(candidate, root.grant_id)) {
        return Restore::corrupt; // an ACTIVE descendant needs a same-batch state first
    }
    root.cleanup_after_grant_id = record.after_grant_id;
    root.cleanup_stamp = stamp;
    return Restore::restored;
}

// Before recovery publishes work, prove all retained relationships, including
// roots whose completed cursor may not skip an active descendant (C-AUTH-026).
bool GrantRestore::consistent(const GrantTable &table) {
    for (uint32_t index = 0; index < table.size(); ++index) {
        const Grant &grant = table.at(index);
        if (!valid_revocation_metadata(table, grant.grant_id, grant.parent_grant_id,
                                       grant.lifecycle, grant.stamp, grant.revoked_by_root_id,
                                       grant.cleanup_after_grant_id, grant.cleanup_through_grant_id,
                                       grant.cleanup_stamp)) {
            return false;
        }
        uint32_t predecessor = 0;
        if (grant.replaced_grant_id != 0U && table.find(grant.replaced_grant_id, predecessor)) {
            return false;
        }
        if (grant.lifecycle == AYNI_GRANT_ACTIVE) {
            for (uint32_t root = 0; root < table.size(); ++root) {
                const Grant &candidate = table.at(root);
                if (is_revocation_root(candidate.grant_id, candidate.revoked_by_root_id) &&
                    grant.grant_id <= candidate.cleanup_after_grant_id &&
                    table.descendant_of(index, candidate.grant_id)) {
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
            if (!table.find(grant.parent_grant_id, parent) || !table.valid_link(index, parent)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace ayni::authority
