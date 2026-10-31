// The call store (persistent_store.h). Journal pairing of a reconciliation
// with its owner command stays in WorkReplay.
#include "foundation/workspace.h"
#include "kernel/service/model/store_limits.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"

#include "work/call/call_records.h"

namespace ayni::kernel {
namespace {

bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == work::call_record_version;
}
uint32_t capacity(const KernelCapacities &capacities) {
    return capacities.call_capacity;
}
uint32_t record_max(const KernelCapacities &capacities) {
    return work::call_record_max(call_limits(capacities));
}

const StoreKind kinds[] = {{work::call_record_type}};
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
            writer.add_record_body({work::call_record_type, work::call_record_version,
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
    return {work::call_record_version, work::call_record_size(record)};
}
void snapshot_write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto index = cursor.index;
    work::write_call_record(model.calls.record(index), body);
}

// Snapshot rows are independently bounded by the covered durable prefix.
// Cross-row references are proved after every section is restored.
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t, const uint8_t *body, uint32_t size, uint16_t,
                                     StoreReadCursor &) {
    work::CallRecord record{};
    uint32_t duplicate = 0;
    if (!work::read_call_record(model.workspace, body, size, record) ||
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

// The new Call closes the Task-owned optional source pair in this same batch.
// Updates to an existing Call cannot replace or supply an initial binding.
bool WorkReplay::pair_tool_acceptance(const work::CallRecord &record) {
    const auto &original = record.identity.authorization;
    if (original.grant_id) {
        if (!pairing_.tool_bind || pairing_.tool_bind->task_id != record.identity.task_id ||
            pairing_.tool_bind->reference.source_call_id != record.identity.call_id ||
            pairing_.tool_bind->reference.selected.grant_id != original.grant_id ||
            pairing_.tool_bind->reference.selected.revision != original.revision ||
            record.contract.category != catalog::CallClass::tool) {
            return false;
        }
        pairing_.tool_bind.reset();
    } else if (pairing_.tool_bind) {
        return false;
    }
    return true;
}

// Journal-only: a first row is an acceptance, while snapshot rows may already
// be terminal. No registration or callback is consulted when restoring facts.
durability::RecordUse WorkReplay::restore_call(const uint8_t *body, uint32_t size,
                                               work::ActionStamp stamp) {
    using durability::RecordUse;
    work::CallRecord record{};
    uint32_t task_index = 0;
    uint32_t call_index = 0;
    if (!work::read_call_record(model_.workspace, body, size, record) ||
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
          (record.state.subscription->phase != work::SubscriptionPhase::starting ||
           record.state.subscription->generation != record.identity.operation_id)) ||
         record.action || record.state.outcome != 0U ||
         (record.state.attachment == work::CallAttachment::foreground
              ? task.call.id != record.identity.call_id
              : record.identity.mode != AYNI_CALL_MODE_MANAGED ||
                    !model_.tasks.has_step(task_index, {record.identity.step_key.data(),
                                                        record.identity.step_key.size()}) ||
                    task.steps_seq != stamp.transition.commit_seq) ||
         task.commit_seq != stamp.transition.commit_seq)) {
        return RecordUse::corrupt;
    }
    if (!exists && !pair_tool_acceptance(record)) {
        return RecordUse::corrupt;
    }
    // Initial foreground admission and detachment pair with Task progress. Once
    // detached, a background generation never owns another current_call link.
    const bool terminal = record.state.outcome != 0U;
    const bool reconciliation = record.state.query.has_value();
    const bool resolved = exists && model_.calls.state(call_index).outcome == AYNI_CALL_ABANDONED &&
                          record.state.outcome != AYNI_CALL_ABANDONED;
    const bool cancelled_task = !work::TaskTable::is_live(task.lifecycle);
    const bool detached_call =
        record.state.subscription.has_value() || record.identity.mode == AYNI_CALL_MODE_MANAGED;
    const bool foreground_before =
        exists && model_.calls.state(call_index).attachment == work::CallAttachment::foreground;
    const bool foreground_after = record.state.attachment == work::CallAttachment::foreground;
    if (detached_call) {
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
            (state.outcome == 0U && state.attachment == work::CallAttachment::foreground &&
             task.call.id != id.call_id) ||
            (state.outcome == 0U && state.attachment == work::CallAttachment::background &&
             task.call.id == id.call_id) ||
            (state.outcome != 0U && state.outcome != AYNI_CALL_ABANDONED &&
             task.call.id == id.call_id)) {
            return false;
        }
        for (uint32_t at = 0; at < id.model_context.sample_count; ++at) {
            const auto &sample = id.model_context.samples[at];
            uint32_t source = 0;
            if (!model_.calls.find(sample.call_id, source) ||
                model_.calls.identity(source).task_id != id.task_id ||
                !model_.calls.state(source).subscription ||
                model_.calls.state(source).created.transition.commit_seq >=
                    state.created.transition.commit_seq ||
                sample.generation > model_.calls.state(source).subscription->generation) {
                return false;
            }
        }
        if (id.authorization.grant_id) {
            const auto admission = model_.task_authority.at(task.slot);
            uint32_t matches = 0;
            for (uint32_t at = 0; at < admission.tool_count; ++at) {
                if (admission.tools[at].source_call_id == id.call_id) {
                    ++matches;
                }
            }
            if (matches != 1U) {
                return false;
            }
        }
        // Both journal and snapshot must retain the actual end of this run.
        // SKIPPED never makes an ordinary/foreground Task ready to decide again.
        if (state.outcome == AYNI_CALL_SKIPPED &&
            ((task.lifecycle != AYNI_TASK_EXPIRED &&
              !(task.lifecycle == AYNI_TASK_CANCELLING &&
                task.call.end == work::TaskCall::End::expire)) ||
             model_.goals.at(task.goal_index).work_profile.kind != AYNI_WORK_BACKGROUND)) {
            return false;
        }
    }
    return task_calls_consistent();
}

// Task references and its independent UNKNOWN overlay must resolve to retained original Calls.
bool WorkReplay::task_calls_consistent() const {
    for (uint32_t index = 0; index < model_.tasks.size(); ++index) {
        const auto &task = model_.tasks.at(index);
        uint32_t call_index = 0;
        if (task.operation_wait &&
            (!model_.calls.find(task.operation_wait->call_id, call_index) ||
             model_.calls.identity(call_index).task_id != task.task_id ||
             static_cast<uint32_t>(model_.calls.contract(call_index).form) !=
                 task.operation_wait->form ||
             task.operation_wait->generation >
                 (model_.calls.state(call_index).subscription
                      ? model_.calls.state(call_index).subscription->generation
                      : model_.calls.identity(call_index).operation_id))) {
            return false;
        }
        if (work::TaskTable::waiting_for_resource(task) &&
            work::TaskTable::is_live(task.lifecycle) &&
            task.call.end == work::TaskCall::End::none) {
            bool queued = false;
            for (uint32_t at = 0; at < model_.calls.capacity(); ++at) {
                queued = queued || (model_.calls.occupied(at) &&
                                    model_.calls.identity(at).task_id == task.task_id &&
                                    model_.calls.state(at).outcome == 0 &&
                                    model_.calls.state(at).attempt.request_id == 0);
            }
            if (!queued) {
                return false;
            }
        }
        if (work::TaskTable::is_live(task.lifecycle) &&
            task.call.end == work::TaskCall::End::none) {
            bool unknown = false;
            for (uint32_t at = 0; at < model_.calls.capacity(); ++at) {
                unknown = unknown || (model_.calls.occupied(at) &&
                                      model_.calls.identity(at).task_id == task.task_id &&
                                      model_.calls.state(at).outcome == AYNI_CALL_ABANDONED);
            }
            if (task.call_unknown != unknown) {
                return false;
            }
        }
        if (task.call.id != 0U && (!model_.calls.find(task.call.id, call_index) ||
                                   model_.calls.identity(call_index).task_id != task.task_id)) {
            return false;
        }
    }
    return true;
}
} // namespace ayni::kernel
