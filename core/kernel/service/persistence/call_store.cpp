// The call store (persistent_store.h). Journal pairing of a reconciliation
// with its owner command stays in WorkReplay.
#include "kernel/service/model/store_limits.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"

#include "work/call/call_records.h"

namespace ayni::kernel {
namespace {

bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == work::call_record_version || version == work::call_skipped_record_version ||
           version == work::call_execution_record_version ||
           version == work::call_tool_calls_record_version ||
           version == work::call_erased_record_version ||
           version == work::call_subscription_record_version ||
           version == work::call_erased_subscription_record_version ||
           version == work::call_scoped_execution_record_version ||
           version == work::call_erased_scoped_execution_record_version;
}
uint32_t capacity(const KernelCapacities &capacities) {
    return capacities.call_capacity;
}
uint32_t record_max(const KernelCapacities &capacities) {
    return work::call_record_max(call_limits(capacities));
}

const StoreKind kinds[] = {{work::call_record_type, always_required}};
uint32_t journal_max(const KernelCapacities &c) {
    return capacity(c) == 0U ? 0U : record_bytes(record_max(c));
}

// One call advances per item/transition. Its full row follows Task progress,
// so a committed batch contains both B4 partners or neither.
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    uint32_t written = 0;
    for (uint32_t index = 0; index < model.calls.capacity(); ++index) {
        if (!model.calls.occupied(index) ||
            model.calls.state(index).latest.transition.commit_seq != stamp.commit_seq) {
            continue;
        }
        const auto record = model.calls.record(index);
        if (++written > 1U || record.state.latest.transition.transition_id != stamp.transition_id) {
            return false;
        }
        auto *at =
            writer.add_record_body({work::call_record_type, work::call_record_version_for(record),
                                    durability::record_required, work::call_record_size(record)});
        if (at == nullptr) {
            return false;
        }
        work::write_call_record(record, at);
    }
    return true;
}

// Skips empty retained slots; the payload carries the journal body.
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    auto &index = cursor.index;
    while (index < model.calls.capacity() && !model.calls.occupied(index)) {
        ++index;
    }
    return index < model.calls.capacity();
}
StoreRecord snapshot_record(const WorkModel &model, const StoreCursor &cursor) {
    const auto index = cursor.index;
    const auto record = model.calls.record(index);
    return {work::call_record_version_for(record), work::call_record_size(record)};
}
void snapshot_write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto index = cursor.index;
    work::write_call_record(model.calls.record(index), body);
}

// Snapshot rows are independently bounded by the covered durable prefix.
// Cross-row references are proved after every section is restored.
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t version, const uint8_t *body, uint32_t size, uint16_t,
                                     StoreReadCursor &) {
    work::CallRecord record{};
    uint32_t duplicate = 0;
    if (!work::read_call_record(body, size, record, version) ||
        record.state.latest.boot_epoch > metadata.boot_epoch ||
        !within_snapshot(record.state.latest.transition, metadata) ||
        (record.state.content_erased &&
         (record.state.content_erased->boot_epoch > metadata.boot_epoch ||
          !within_snapshot(record.state.content_erased->transition, metadata))) ||
        model.calls.find(record.identity.call_id, duplicate)) {
        return durability::BatchCheck::corrupt;
    }
    const auto status = model.calls.restore_validated(record);
    return status == AYNI_OK                  ? durability::BatchCheck::ok
           : status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                              : durability::BatchCheck::corrupt;
}

} // namespace

const PersistentStore call_store{work::call_record_type,
                                 kinds,
                                 1,
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

// Journal-only: a first row is an acceptance, while snapshot rows may already
// be terminal. No registration or callback is consulted when restoring facts.
durability::RecordUse WorkReplay::restore_call(uint16_t version, const uint8_t *body, uint32_t size,
                                               work::ActionStamp stamp) {
    using durability::RecordUse;
    work::CallRecord record{};
    uint32_t task_index = 0;
    uint32_t call_index = 0;
    if (!work::read_call_record(body, size, record, version) ||
        record.state.latest.boot_epoch != stamp.boot_epoch ||
        record.state.latest.transition.commit_seq != stamp.transition.commit_seq ||
        record.state.latest.transition.transition_id != stamp.transition.transition_id ||
        !model_.tasks.find(record.identity.task_id, task_index)) {
        return RecordUse::corrupt;
    }
    const auto &task = model_.tasks.at(task_index);
    if (task.owner_instance_id != record.identity.owner_instance_id ||
        task.created_transition_id >= record.state.created.transition.transition_id) {
        return RecordUse::corrupt;
    }
    const bool exists = model_.calls.find(record.identity.call_id, call_index);
    if (!exists &&
        (record.state.created.transition.commit_seq != stamp.transition.commit_seq ||
         (record.state.subscription &&
          (record.state.subscription->attachment != work::SubscriptionAttachment::foreground ||
           record.state.subscription->phase != work::SubscriptionPhase::starting ||
           record.state.subscription->generation != record.identity.operation_id)) ||
         record.action || record.state.outcome != 0U || task.call.id != record.identity.call_id ||
         task.commit_seq != stamp.transition.commit_seq)) {
        return RecordUse::corrupt;
    }
    // Initial foreground admission and detachment pair with Task progress. Once
    // detached, a background generation never owns another current_call link.
    const bool terminal = record.state.outcome != 0U;
    const bool reconciliation = record.state.query.has_value();
    const bool resolved = exists && model_.calls.state(call_index).outcome == AYNI_CALL_ABANDONED &&
                          record.state.outcome != AYNI_CALL_ABANDONED;
    const bool cancelled_task = !work::TaskTable::is_live(task.lifecycle);
    const bool subscription = record.state.subscription.has_value();
    const bool foreground_before = subscription && exists &&
                                   model_.calls.state(call_index).subscription &&
                                   model_.calls.state(call_index).subscription->attachment ==
                                       work::SubscriptionAttachment::foreground;
    const bool foreground_after = subscription && record.state.subscription->attachment ==
                                                      work::SubscriptionAttachment::foreground;
    if (subscription) {
        const bool detached = foreground_before && !foreground_after;
        if ((foreground_after && task.call.id != record.identity.call_id) ||
            (detached && (task.commit_seq != stamp.transition.commit_seq ||
                          (record.state.outcome != AYNI_CALL_ABANDONED &&
                           task.call.id == record.identity.call_id)))) {
            return RecordUse::corrupt;
        }
    } else if ((terminal && (!reconciliation || (resolved && !cancelled_task)) &&
                task.commit_seq != stamp.transition.commit_seq) ||
               (!terminal && task.call.id != record.identity.call_id)) {
        return RecordUse::corrupt;
    }
    if (reconciliation && record.state.query->phase == work::CallQuery::Phase::requested) {
        if (pairing_.target_command) {
            return RecordUse::corrupt;
        }
        pairing_.target_command = BatchPairing::TargetCommandPair{{AYNI_INTENT_CALL_QUERY_RESULT,
                                                                   record.state.query->generation,
                                                                   record.identity.call_id}};
    }
    const auto status = model_.calls.restore_validated(record);
    return status == AYNI_OK                  ? RecordUse::applied
           : status == AYNI_CAPACITY_EXCEEDED ? RecordUse::no_room
                                              : RecordUse::corrupt;
}

bool WorkReplay::pair_target_command(const CommandMapping &mapping, ayni_bytes_v1 canonical) {
    TargetCommand command{};
    const auto *binding = model_.principals.binding();
    if (!pairing_.target_command || pairing_.target_command->command_seen ||
        !read_target_command(canonical, command) ||
        !(command == pairing_.target_command->request) || binding == nullptr ||
        mapping.principal_id != binding->principal_id ||
        binding->stamp.commit_seq >= mapping.commit_seq) {
        return false;
    }
    pairing_.target_command->command_seen = true;
    return true;
}

// Stable foreign keys, never moving slots, pair selected work with the retained
// ledger. A cancelled Task may retain UNKNOWN facts after local tracking ended.
bool WorkReplay::calls_consistent(uint64_t boot_epoch_limit) const {
    for (uint32_t index = 0; index < model_.calls.capacity(); ++index) {
        if (!model_.calls.occupied(index)) {
            continue;
        }
        const auto &id = model_.calls.identity(index);
        const auto &state = model_.calls.state(index);
        uint32_t task_index = 0;
        if (state.latest.boot_epoch > boot_epoch_limit ||
            !model_.tasks.find(id.task_id, task_index)) {
            return false;
        }
        const auto &task = model_.tasks.at(task_index);
        if (state.content_erased) {
            const auto *sources =
                model_.provenance.find(work::ProvenanceKind::task, task.slot, task.task_id);
            if (sources == nullptr || sources->content != work::ContentState::erased ||
                state.content_erased->boot_epoch > boot_epoch_limit ||
                state.content_erased->transition.commit_seq < sources->stamp.commit_seq ||
                state.content_erased->transition.commit_seq >
                    model_.privacy.record().stamp.commit_seq) {
                return false;
            }
        }
        if (id.owner_instance_id != task.owner_instance_id ||
            task.created_transition_id >= state.created.transition.transition_id ||
            (state.outcome == 0U &&
             (!state.subscription ||
              state.subscription->attachment == work::SubscriptionAttachment::foreground) &&
             task.call.id != id.call_id) ||
            (state.outcome == 0U && state.subscription &&
             state.subscription->attachment == work::SubscriptionAttachment::background &&
             task.call.id == id.call_id) ||
            (state.outcome != 0U && state.outcome != AYNI_CALL_ABANDONED &&
             task.call.id == id.call_id)) {
            return false;
        }
        // Both journal and snapshot must retain the actual end of this run.
        // SKIPPED never makes an ordinary/foreground Task ready to decide again.
        if (state.outcome == AYNI_CALL_SKIPPED &&
            (task.lifecycle != AYNI_TASK_EXPIRED || task.call.id != 0U ||
             model_.goals.at(task.goal_index).work_profile.kind != AYNI_WORK_BACKGROUND)) {
            return false;
        }
    }
    for (uint32_t index = 0; index < model_.tasks.size(); ++index) {
        const auto &task = model_.tasks.at(index);
        uint32_t call_index = 0;
        if (task.subscription_wait &&
            (!model_.calls.find(task.subscription_wait->subscription_id, call_index) ||
             model_.calls.identity(call_index).task_id != task.task_id ||
             !model_.calls.state(call_index).subscription ||
             task.subscription_wait->generation >
                 model_.calls.state(call_index).subscription->generation)) {
            return false;
        }
        if (task.call.id != 0U && (!model_.calls.find(task.call.id, call_index) ||
                                   model_.calls.identity(call_index).task_id != task.task_id)) {
            return false;
        }
    }
    return true;
}
} // namespace ayni::kernel
