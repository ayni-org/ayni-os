// The Grant family owns its codecs, bounds, journal order and parent-first snapshot walk.
// Cross-store command/revocation pairing stays with WorkReplay (ADR-0059).
#include "authority/grant/grant_records.h"
#include "foundation/kernel_id.h"
#include "kernel/service/persistence/persistent_store.h"
namespace ayni::kernel {
namespace {
const StoreKind kinds[] = {{authority::grant_record_type},
                           {authority::grant_state_record_type},
                           {authority::grant_revocation_record_type},
                           {authority::grant_revocation_progress_record_type}};
// Schema 3 separates complete creation and retained snapshot history.
bool knows(uint16_t type, uint16_t version, WorkRecordContext context) {
    if (type == authority::grant_record_type) {
        return version == (context == WorkRecordContext::journal
                               ? authority::grant_record_version
                               : authority::grant_snapshot_record_version);
    }
    if (context != WorkRecordContext::journal) {
        return false;
    }
    if (type == authority::grant_state_record_type) {
        return version == authority::grant_state_record_version;
    }
    if (type == authority::grant_revocation_record_type) {
        return version == authority::grant_revocation_record_version;
    }
    return type == authority::grant_revocation_progress_record_type &&
           version == authority::grant_revocation_progress_record_version;
}
uint32_t capacity(const KernelCapacities &c) {
    return c.grant_capacity;
}
uint32_t record_max(const KernelCapacities &c) {
    return authority::grant_snapshot_record_max(c.authority_scope_max_bytes);
}
uint32_t journal_max(const KernelCapacities &capacities) {
    uint32_t bytes = 0;
    // Reserve all authority records conservatively, including the bounded
    // startup expiry sweep. This avoids a second admission budget per kind.
    const uint32_t batch_grants = capacities.goal_requirement_capacity < capacities.grant_capacity
                                      ? capacities.goal_requirement_capacity
                                      : capacities.grant_capacity;
    bytes += batch_grants *
             record_bytes(authority::grant_record_max(capacities.authority_scope_max_bytes));
    bytes += capacities.grant_capacity * record_bytes(authority::grant_state_record_size);
    bytes += record_bytes(authority::grant_revocation_record_size);
    bytes += record_bytes(authority::grant_state_record_size);
    bytes += record_bytes(authority::grant_revocation_progress_record_size);
    return bytes;
}

// One store subsequence: accepted root control, changed facts, then cleanup progress.
class GrantJournal {
  public:
    // Borrow one immutable model/transition and its output buffer for this encode.
    GrantJournal(const WorkModel &model, foundation::TransitionStamp stamp,
                 durability::BatchWriter &writer)
        : model_(model), stamp_(stamp), writer_(writer) {}
    // Preserve root-control, changed-fact, then progress order for WorkReplay pairing.
    bool encode() { return revocation() && grants() && revocation_progress(); }

  private:
    uint8_t *body(uint16_t type, uint16_t version, uint32_t size) {
        auto *at = writer_.add_record_body({type, version, durability::record_required, size});
        ok_ = ok_ && at != nullptr;
        return at;
    }
    // Root control comes from its exact command, including TARGET_GONE without a row.
    bool revocation() {
        const auto *mapping = model_.commands.mapped_in(stamp_.commit_seq);
        if (mapping == nullptr || mapping->kind != AYNI_INTENT_GRANT_REVOKE) {
            return ok_;
        }
        authority::GrantTable::RevokePlan plan{mapping->object.id, mapping->object.revision,
                                               mapping->effect, 0, 0};
        if (mapping->effect == AYNI_INTENT_EFFECT_CHANGED) {
            uint32_t index = 0;
            if (!model_.grants.find(mapping->object.id, index)) {
                return false;
            }
            const auto &grant = model_.grants.at(index);
            if (!authority::is_revocation_root(grant.grant_id, grant.revoked_by_root_id) ||
                grant.stamp.commit_seq != stamp_.commit_seq) {
                return false;
            }
            plan.cleanup_after_grant_id = grant.cleanup_after_grant_id;
            plan.cleanup_through_grant_id = grant.cleanup_through_grant_id;
        }
        auto *at = body(authority::grant_revocation_record_type,
                        authority::grant_revocation_record_version,
                        authority::grant_revocation_record_size);
        if (at != nullptr) {
            authority::write_grant_revocation_record(plan, at);
        }
        return ok_;
    }
    // Encode only this transition; explicit roots already have their paired revocation.
    bool grants() {
        for (uint32_t index = 0; index < model_.grants.size(); ++index) {
            const auto &grant = model_.grants.at(index);
            if (grant.stamp.commit_seq != stamp_.commit_seq) {
                continue;
            }
            // Explicit root revocation is encoded by its paired type 15.
            if (grant.lifecycle == AYNI_GRANT_REVOKED &&
                authority::is_revocation_root(grant.grant_id, grant.revoked_by_root_id)) {
                continue;
            }
            const bool created = grant.created_stamp.commit_seq == stamp_.commit_seq;
            const uint32_t size = created ? authority::grant_record_size(grant)
                                          : authority::grant_state_record_size;
            uint8_t *at =
                body(created ? authority::grant_record_type : authority::grant_state_record_type,
                     created ? authority::grant_record_version : authority::grant_state_record_version,
                     size);
            if (at == nullptr) {
                continue;
            }
            if (created) {
                authority::write_grant_record(grant, model_.grants.source_domain(index),
                                              model_.grants.scope(index), at);
            } else {
                authority::write_grant_state_record(grant, at);
            }
        }
        return ok_;
    }
    // Cleanup follows an optional changed descendant; root creation carries its own cursor.
    bool revocation_progress() {
        for (uint32_t index = 0; index < model_.grants.size(); ++index) {
            const auto &grant = model_.grants.at(index);
            if (grant.cleanup_stamp.commit_seq != stamp_.commit_seq ||
                grant.stamp.commit_seq == stamp_.commit_seq) {
                continue;
            }
            auto *at = body(authority::grant_revocation_progress_record_type,
                            authority::grant_revocation_progress_record_version,
                            authority::grant_revocation_progress_record_size);
            if (at != nullptr) {
                authority::write_grant_revocation_progress_record(
                    {grant.grant_id, grant.cleanup_after_grant_id, grant.cleanup_through_grant_id,
                     0},
                    at);
            }
        }
        return ok_;
    }
    const WorkModel &model_;            // Frozen transition facts, borrowed for one encode.
    foundation::TransitionStamp stamp_; // Exact transition/commit pair being encoded.
    durability::BatchWriter &writer_;   // Borrowed batch buffer; this adapter never submits I/O.
    bool ok_ = true; // Sticky encoding failure; false prevents a completed batch.
};
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    return GrantJournal(model, stamp, writer).encode();
}
// C-STORE-015: depth order survives terminal slot reuse; parent rows always precede children.
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    return next_snapshot_at_depth(model.grants, AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX, cursor.order,
                                  cursor.index);
}
StoreRecord snapshot_record(const WorkModel &model, const StoreCursor &cursor) {
    const auto index = cursor.index;
    const auto &grant = model.grants.at(index);
    return {authority::grant_snapshot_record_version, authority::grant_snapshot_record_size(grant)};
}
void snapshot_write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto index = cursor.index;
    authority::write_grant_snapshot_record(
        model.grants.at(index), model.grants.source_domain(index), model.grants.scope(index), body);
}

durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t, const uint8_t *body, uint32_t size, uint16_t,
                                     StoreReadCursor &) {
    authority::GrantSnapshotRecord record{};
    const auto *binding = model.principals.binding();
    if (!authority::read_grant_snapshot_record(body, size, record) ||
        !within_snapshot(record.created_stamp, metadata) ||
        !within_snapshot(record.stamp, metadata) ||
        ((record.cleanup_stamp.transition_id != 0U || record.cleanup_stamp.commit_seq != 0U) &&
         !within_snapshot(record.cleanup_stamp, metadata)) ||
        binding == nullptr || binding->principal_id != record.create.grant.owner_principal_id ||
        binding->binding_revision != record.create.grant.owner_binding_revision ||
        binding->stamp.commit_seq >= record.created_stamp.commit_seq ||
        binding->stamp.transition_id >= record.created_stamp.transition_id ||
        model.agents.find_instance(record.create.grant.subject_instance_id) == nullptr) {
        return durability::BatchCheck::corrupt;
    }
    return snapshot_restore_check(model.grants.restore_snapshot(record));
}
} // namespace
const PersistentStore grant_store{authority::grant_record_type,
                                  kinds,
                                  static_cast<uint32_t>(sizeof(kinds) / sizeof(kinds[0])),
                                  knows,
                                  StorePosition::before_work,
                                  capacity,
                                  record_max,
                                  journal_max,
                                  journal,
                                  snapshot_next,
                                  snapshot_record,
                                  snapshot_write,
                                  advance_store_index,
                                  snapshot_read};
} // namespace ayni::kernel
