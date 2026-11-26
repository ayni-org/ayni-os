// ADR-0115: retirement is derived from the existing work replacement batch (zero new WAL).
// Only explicit query/closure transitions have a journal delta; snapshots hold complete rows.
#include "foundation/kernel_id.h"
#include "kernel/service/model/store_limits.h"
#include "kernel/service/persistence/work_replay.h"
#include "work/operation/operation_records.h"
namespace ayni::kernel {
namespace {
// Full rows are snapshot-only; accepting a full row from a journal would bypass retirement.
bool knows(uint16_t type, uint16_t version, WorkRecordContext context) {
    return version == work::operation_record_version &&
           (context == WorkRecordContext::snapshot ? type == work::unresolved_op_record_type
                                                   : type == work::unresolved_op_state_record_type);
}
uint32_t capacity(const KernelCapacities &c) {
    return c.unresolved_op_capacity;
}
uint32_t record_max(const KernelCapacities &c) {
    return work::operation_record_max(operation_limits(c));
}
uint32_t journal_max(const KernelCapacities &c) {
    return c.unresolved_op_capacity ? record_bytes(work::operation_state_bytes) : 0U;
}
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    // Retirement has no delta: the existing work replacement is its atomic durable proof.
    const auto &change = model.unresolved_ops.journal_change();
    if (!change || change->stamp.transition.commit_seq != stamp.commit_seq) {
        return true;
    }
    if (change->stamp.transition.transition_id != stamp.transition_id) {
        return false;
    }
    auto *body = writer.add_record_body({work::unresolved_op_state_record_type,
                                         work::operation_record_version,
                                         durability::record_required, work::operation_state_bytes});
    if (!body) {
        return false;
    }
    work::write_operation_change(*change, body);
    return true;
}
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    while (cursor.index < model.unresolved_ops.capacity() &&
           !model.unresolved_ops.occupied(cursor.index)) {
        ++cursor.index;
    }
    return cursor.index < model.unresolved_ops.capacity();
}
StoreRecord snapshot_record(const WorkModel &model, const StoreCursor &cursor) {
    return {work::operation_record_version,
            work::operation_record_size(model.unresolved_ops.record(cursor.index))};
}
void snapshot_write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    work::write_operation_record(model.unresolved_ops.record(cursor.index), body);
}
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t, const uint8_t *body, uint32_t size, uint16_t,
                                     StoreReadCursor &) {
    // Snapshot metadata bounds every stamp; global identity relations follow after all stores.
    work::OperationRecord record{};
    if (!work::read_operation_record(model.workspace, body, size, record) ||
        record.state.latest.boot_epoch > metadata.boot_epoch ||
        !within_snapshot(record.state.latest.transition, metadata)) {
        return durability::BatchCheck::corrupt;
    }
    const auto status = model.unresolved_ops.restore_snapshot_entry(record);
    return status == AYNI_OK                  ? durability::BatchCheck::ok
           : status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                              : durability::BatchCheck::corrupt;
}
const StoreKind kinds[] = {{work::unresolved_op_record_type},
                           {work::unresolved_op_state_record_type}};
} // namespace
const PersistentStore operation_store{work::unresolved_op_record_type,
                                      kinds,
                                      sizeof(kinds) / sizeof(kinds[0]),
                                      knows,
                                      StorePosition::after_work,
                                      capacity,
                                      record_max,
                                      journal_max,
                                      journal,
                                      snapshot_next,
                                      snapshot_record,
                                      snapshot_write,
                                      advance_store_index,
                                      snapshot_read};

// Owner command changes are paired exactly once; transport updates must stand alone in a batch.
durability::RecordUse WorkReplay::restore_unresolved_op_state(const uint8_t *body, uint32_t size,
                                                              work::ActionStamp stamp) {
    using durability::RecordUse;
    work::OperationChange change{};
    uint32_t index = 0;
    if (!work::read_operation_change(body, size, change) ||
        change.stamp.boot_epoch != stamp.boot_epoch ||
        change.stamp.transition.commit_seq != stamp.transition.commit_seq ||
        change.stamp.transition.transition_id != stamp.transition.transition_id ||
        !model_.unresolved_ops.find(change.call_id, index) || !pairing_.complete() ||
        batch_seq_ == stamp.transition.commit_seq) {
        return RecordUse::corrupt;
    }
    const auto revision = model_.unresolved_ops.state(index).revision;
    if (model_.unresolved_ops.restore_journal_change(change) != AYNI_OK) {
        return RecordUse::corrupt;
    }
    if (change.kind == work::OperationChange::Kind::query_requested ||
        change.kind == work::OperationChange::Kind::acknowledged) {
        pairing_.target_command = BatchPairing::TargetCommandPair{
            {change.kind == work::OperationChange::Kind::query_requested
                 ? AYNI_INTENT_UNRESOLVED_OP_QUERY
                 : AYNI_INTENT_UNRESOLVED_OP_ACKNOWLEDGE,
             revision, change.call_id}};
    } else {
        pairing_.standalone = BatchPairing::Standalone::operation_update;
    }
    return RecordUse::applied;
}

// Historical Task IDs are references only. All live objects and both identity namespaces stay
// disjoint.
bool WorkReplay::operations_consistent(uint64_t boot_epoch_limit) const {
    const auto &ledger = model_.unresolved_ops;
    for (uint32_t at = 0; at < ledger.capacity(); ++at) {
        if (!ledger.occupied(at)) {
            continue;
        }
        const auto &id = ledger.identity(at);
        uint32_t index = 0;
        if (ledger.state(at).latest.boot_epoch > boot_epoch_limit ||
            model_.agents.find_instance(id.owner_instance_id) == nullptr ||
            model_.tasks.find(id.task_id, index)) {
            return false;
        }
        for (const auto value : {id.call_id, id.operation_id}) {
            if (foundation::id_boot_epoch(value) > boot_epoch_limit ||
                model_.goals.find(value, index) || model_.tasks.find(value, index) ||
                model_.grants.find(value, index) || model_.memories.find(value, index) ||
                model_.agents.find_instance(value)) {
                return false;
            }
            for (uint32_t scene = 0; scene < model_.scenes.state_count(); ++scene) {
                if (model_.scenes.state(scene).object_id == value) {
                    return false;
                }
            }
            for (uint32_t call = 0; call < model_.calls.capacity(); ++call) {
                if (!model_.calls.occupied(call)) {
                    continue;
                }
                const auto &active = model_.calls.identity(call);
                const auto &state = model_.calls.state(call);
                if (value == active.call_id || value == active.operation_id ||
                    (state.subscription && value == state.subscription->generation)) {
                    return false;
                }
            }
        }
    }
    return true;
}
} // namespace ayni::kernel
