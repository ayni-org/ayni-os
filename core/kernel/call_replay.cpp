#include "kernel/work_replay.h"

#include "work/call/call_records.h"

namespace ayni::kernel {
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
         record.action || record.state.outcome != 0U || task.call.id != record.identity.call_id ||
         task.commit_seq != stamp.transition.commit_seq)) {
        return RecordUse::corrupt;
    }
    // A known result releases the selected call only with the Task in this batch.
    // UNKNOWN keeps the reference unless a user cancellation has already drained it.
    const bool terminal = record.state.outcome != 0U;
    const bool reconciliation = record.state.query.has_value();
    const bool resolved = exists && model_.calls.state(call_index).outcome == AYNI_CALL_ABANDONED &&
                          record.state.outcome != AYNI_CALL_ABANDONED;
    const bool cancelled_task = !work::TaskTable::is_live(task.lifecycle);
    if ((terminal && (!reconciliation || (resolved && !cancelled_task)) &&
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
        if (id.owner_instance_id != task.owner_instance_id ||
            task.created_transition_id >= state.created.transition.transition_id ||
            (state.outcome == 0U && task.call.id != id.call_id) ||
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
        if (task.call.id != 0U && (!model_.calls.find(task.call.id, call_index) ||
                                   model_.calls.identity(call_index).task_id != task.task_id)) {
            return false;
        }
    }
    return true;
}
} // namespace ayni::kernel
