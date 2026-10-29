#include "durability/batch/record_body.h"
#include "foundation/little_endian.h"
#include "kernel/service/persistence/persistent_store.h"
#include "work/task/task_authority_records.h"
#include "work/task/task_records.h"
namespace ayni::kernel {
namespace {

durability::RecordHeader header(uint16_t type, uint16_t version, uint32_t length) {
    return {type, version, durability::record_required, length};
}
// Owns this family's encoding in the original schema 2 journal order.
class Journal {
  public:
    Journal(const WorkModel &model, foundation::TransitionStamp stamp,
            durability::BatchWriter &writer)
        : model_(model), stamp_(stamp), writer_(writer),
          command_(model.commands.mapped_in(stamp.commit_seq)) {}
    bool encode() { return tasks(); }

  private:
    uint8_t *body(uint16_t type, uint16_t version, uint32_t length) {
        uint8_t *at = writer_.add_record_body(header(type, version, length));
        ok_ = ok_ && at != nullptr;
        return at;
    }
    // Encode creation, then steps/checkpoint, then final state and its admission.
    // This order is part of the schema 2 cross-record replay proof.
    bool tasks() {
        const work::TaskTable &tasks = model_.tasks;
        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const work::Task &task = tasks.at(index);
            if (task.commit_seq != stamp_.commit_seq ||
                task.created_transition_id != stamp_.transition_id) {
                continue;
            }
            uint8_t *at = body(work::task_record_type, work::task_record_version,
                               work::task_creation_record_size(task));
            if (at != nullptr) {
                work::write_task_record(task, at);
            }
        }
        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const work::Task &task = tasks.at(index);
            if (task.steps_seq == stamp_.commit_seq) {
                const work::TaskStepRange range{task.steps_before,
                                                task.step_count - task.steps_before};
                uint8_t *at = body(work::task_steps_record_type, work::task_steps_record_version,
                                   work::task_steps_record_size(tasks, index, range));
                if (at != nullptr) {
                    work::write_task_steps_record(tasks, index, range, at);
                }
            }
            if (task.checkpoint_seq == stamp_.commit_seq) {
                const bool sources = model_.provenance.find(
                    work::ProvenanceKind::task, task.slot, task.task_id) != nullptr;
                uint8_t *at = body(work::task_checkpoint_record_type,
                                   work::task_checkpoint_record_version,
                                   work::task_checkpoint_record_size(task));
                if (at != nullptr) {
                    work::write_task_checkpoint_record(task, tasks.checkpoint(index), at, sources);
                }
            }
        }
        for (uint32_t index = 0; index < tasks.size(); ++index) {
            const work::Task &task = tasks.at(index);
            if (task.commit_seq != stamp_.commit_seq) {
                continue;
            }
            work::TaskAuthorityRecord admission{};
            if (!model_.task_authority.read(task.slot, task.task_id, admission)) {
                return false; // Schema 2 never infers authorization from absent metadata.
            }
            const bool changed = admission.stamp.commit_seq == stamp_.commit_seq;
            const auto *command = command_;
            const auto change = command != nullptr &&
                                        command->kind == AYNI_INTENT_TASK_RAISE_MODEL_LIMIT
                                    ? work::TaskRecordChange::raise_limit
                                : command != nullptr && command->kind == AYNI_INTENT_TASK_RESUME_WAIT
                                    ? work::TaskRecordChange::resume_wait
                                : changed && goal_reauthorized_by(command, task.goal_id)
                                    ? work::TaskRecordChange::reauthorization
                                : changed && task.created_transition_id != stamp_.transition_id
                                    ? work::TaskRecordChange::authority
                                    : work::TaskRecordChange::state;
            uint8_t *at = body(work::task_state_record_type, work::task_state_record_version,
                               work::task_state_record_size);
            if (at != nullptr) {
                work::write_task_state_record(task, at, change);
            }
            if (changed && !task_authority(admission)) {
                return false;
            }
        }
        return ok_;
    }
    // Metadata changes share this exact transition; no cross-batch substitution.
    bool task_authority(const work::TaskAuthorityRecord &record) {
        if (record.stamp.transition_id != stamp_.transition_id) {
            return false;
        }
        auto *at = body(work::task_authority_record_type, work::task_authority_record_version,
                        work::task_authority_record_max(record.count));
        if (at != nullptr) {
            work::write_task_authority_record(record, at);
        }
        return ok_;
    }
    const WorkModel &model_;
    foundation::TransitionStamp stamp_;
    durability::BatchWriter &writer_;
    const CommandMapping *command_; // Immutable exact mapping for this synchronous encoding.
    bool ok_ = true;
};
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    return Journal(model, stamp, writer).encode();
}
// Only the current body for each context is accepted; semantics live in the body.
bool knows(uint16_t type, uint16_t version, WorkRecordContext context) {
    if (type == work::task_record_type) {
        return version == (context == WorkRecordContext::journal
                               ? work::task_record_version : work::task_snapshot_record_version);
    }
    if (type == work::task_state_record_type) {
        return version == work::task_state_record_version;
    }
    if (type == work::task_checkpoint_record_type) {
        return version == work::task_checkpoint_record_version;
    }
    return type == work::task_steps_record_type ? version == work::task_steps_record_version
                                                : version == work::task_authority_record_version;
}
uint32_t capacity(const KernelCapacities &c) {
    return c.task_capacity;
}
// A page must hold every possible part independently, including final state.
uint32_t record_max(const KernelCapacities &c) {
    uint32_t result = work::task_snapshot_record_size;
    const uint32_t sizes[] = {work::task_state_max_size, work::task_steps_record_max(1),
                              work::task_checkpoint_record_max(c.task_checkpoint_max_bytes),
                              work::task_authority_record_max(c.goal_requirement_capacity)};
    for (const auto size : sizes) {
        if (size > result) {
            result = size;
        }
    }
    return result;
}
// A retained object contributes creation, every bounded step, optional checkpoint,
// final state and exactly one independent authority record.
uint64_t payload_max(const KernelCapacities &c) {
    return uint64_t{c.task_capacity} *
           (record_bytes(work::task_snapshot_record_size) +
            record_bytes(work::task_state_max_size) +
            uint64_t{c.task_step_capacity} * record_bytes(work::task_steps_record_max(1)) +
            record_bytes(work::task_checkpoint_record_max(c.task_checkpoint_max_bytes)) +
            record_bytes(work::task_authority_record_max(c.goal_requirement_capacity)));
}
// One decision can add at most its output capacity of step keys; lifecycle
// propagation can update every Task state and its independent authority.
uint32_t journal_max(const KernelCapacities &c) {
    const auto steps = c.task_step_capacity < c.policy_intent_capacity ? c.task_step_capacity
                                                                       : c.policy_intent_capacity;
    return record_bytes(work::task_record_max_size) +
           record_bytes(work::task_steps_record_max(steps)) +
           record_bytes(work::task_checkpoint_record_max(c.task_checkpoint_max_bytes)) +
           c.task_capacity *
               (record_bytes(work::task_state_max_size) +
                record_bytes(work::task_authority_record_max(c.goal_requirement_capacity)));
}
// Empty steps/checkpoint parts are skipped without consuming a whole record.
bool next(const WorkModel &model, StoreCursor &cursor) {
    if (cursor.index >= model.tasks.size()) {
        return false;
    }
    const auto &task = model.tasks.at(cursor.index);
    if (cursor.part == StorePart::steps && cursor.subindex == task.step_count) {
        cursor.part = StorePart::checkpoint;
    }
    if (cursor.part == StorePart::checkpoint && task.checkpoint_writer_version == 0) {
        cursor.part = StorePart::state;
    }
    return true;
}
// Record type and version belong to the selected part, not the family root.
// Provenance markers retain the same complete-source requirement as before.
StoreRecord shape(const WorkModel &model, const StoreCursor &cursor) {
    const auto &task = model.tasks.at(cursor.index);
    switch (cursor.part) {
    case StorePart::row:
        return {work::task_snapshot_record_version, work::task_snapshot_record_size};
    case StorePart::steps:
        return {work::task_steps_record_version,
                work::task_steps_record_size(model.tasks, cursor.index, {cursor.subindex, 1}),
                work::task_steps_record_type};
    case StorePart::checkpoint:
        return {work::task_checkpoint_record_version,
                work::task_checkpoint_record_size(task), work::task_checkpoint_record_type};
    case StorePart::state: {
        return {work::task_state_record_version, work::task_state_record_size,
                work::task_state_record_type};
    }
    case StorePart::authority:
        return {work::task_authority_record_version,
                work::task_authority_record_max(model.task_authority.at(task.slot).count),
                work::task_authority_record_type};
    }
    return {};
}
// Write the already selected part; selection and page placement cannot mutate work.
void write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto index = cursor.index;
    const auto &task = model.tasks.at(index);
    const bool sources = model.provenance.find(
        work::ProvenanceKind::task, task.slot, task.task_id) != nullptr;
    switch (cursor.part) {
    case StorePart::row:
        work::write_task_snapshot_record(task, body, sources);
        break;
    case StorePart::steps:
        work::write_task_steps_record(model.tasks, index, {cursor.subindex, 1}, body);
        break;
    case StorePart::checkpoint:
        work::write_task_checkpoint_record(task, model.tasks.checkpoint(index), body, sources);
        break;
    case StorePart::state:
        work::write_task_state_record(task, body);
        break;
    case StorePart::authority:
        work::write_task_authority_record(model.task_authority.at(task.slot), body);
        break;
    }
}
// Only consumed records advance the part cursor. An empty step/checkpoint
// is skipped by next(), so repeated page selection never loses a key.
void advance(const WorkModel &, StoreCursor &cursor) {
    switch (cursor.part) {
    case StorePart::row:
        cursor.part = StorePart::steps;
        break;
    case StorePart::steps:
        ++cursor.subindex;
        break;
    case StorePart::checkpoint:
        cursor.part = StorePart::state;
        break;
    case StorePart::state:
        cursor.part = StorePart::authority;
        break;
    case StorePart::authority:
        ++cursor.index;
        cursor.part = StorePart::row;
        cursor.subindex = 0;
        break;
    }
}
// Restore identity and original progress stamps before any child records.
// A second creation cannot interrupt an unfinished object.
durability::BatchCheck task(WorkModel &model, const durability::SnapshotMetadata &metadata,
                            StoreReadCursor &cursor, uint16_t, const uint8_t *body,
                            uint32_t size) {
    work::TaskSnapshotRecord snapshot{};
    if (cursor.index != work::TaskTable::no_index ||
        !work::read_task_snapshot_record(body, size, snapshot)) {
        return durability::BatchCheck::corrupt;
    }
    auto &record = snapshot.creation;
    uint32_t goal_index = 0;
    if (!model.goals.find(record.task.goal_id, goal_index) ||
        model.goals.at(goal_index).owner_instance_id != record.task.owner_instance_id) {
        return durability::BatchCheck::corrupt;
    }
    if (!within_snapshot(snapshot.stamp, metadata)) {
        return durability::BatchCheck::corrupt;
    }
    record.task.goal_index = goal_index;
    const work::TaskTable::Restore restore = model.tasks.restore_snapshot(
        record.task, record.task_id, record.replaced_task_id, snapshot.stamp, snapshot.steps_seq,
        snapshot.checkpoint_seq, snapshot.steps_before);
    if (restore == work::TaskTable::Restore::restored) {
        static_cast<void>(model.tasks.find(record.task_id, cursor.index));
        cursor.checkpoint_seen = false;
        // Erased work may never have written a checkpoint. Its REQUIRED Task marker
        // reserves the source footer independently of the optional checkpoint record.
        if (record.provenance_required &&
            !model.provenance.expect(work::ProvenanceKind::task, model.tasks.at(cursor.index).slot,
                                     record.task_id)) {
            return durability::BatchCheck::corrupt;
        }
    }
    return snapshot_restore_check(restore);
}
// Each snapshot step contains one original-position key and precedes checkpoint.
durability::BatchCheck task_steps(WorkModel &model, const durability::SnapshotMetadata &,
                                  StoreReadCursor &cursor, const uint8_t *body, uint32_t size) {
    if (cursor.index == work::TaskTable::no_index || cursor.checkpoint_seen) {
        return durability::BatchCheck::corrupt;
    }
    const work::Task &task = model.tasks.at(cursor.index);
    if (task.steps_seq == 0U) {
        return durability::BatchCheck::corrupt;
    }
    uint64_t task_id = 0;
    work::TaskTable::Restore restore = work::TaskTable::Restore::corrupt;
    uint32_t seen = 0;
    const bool read =
        work::read_task_steps_record(body, size, task_id, [&](uint32_t step, ayni_string_v1 key) {
            if (task_id != task.task_id || seen++ != 0U) {
                return false;
            }
            restore = model.tasks.restore_step(cursor.index, step, key, {0, task.steps_seq});
            return restore == work::TaskTable::Restore::restored;
        });
    return read ? durability::BatchCheck::ok
                : snapshot_restore_check(restore == work::TaskTable::Restore::no_room
                                             ? restore
                                             : work::TaskTable::Restore::corrupt);
}
// One optional checkpoint retains the original writer and content-source marker.
durability::BatchCheck task_checkpoint(WorkModel &model, const durability::SnapshotMetadata &,
                                       StoreReadCursor &cursor, uint16_t,
                                       const uint8_t *body, uint32_t size) {
    work::TaskCheckpointRecord record{};
    if (cursor.index == work::TaskTable::no_index || cursor.checkpoint_seen ||
        !work::read_task_checkpoint_record(body, size, record)) {
        return durability::BatchCheck::corrupt;
    }
    const work::Task &task = model.tasks.at(cursor.index);
    if (record.task_id != task.task_id || task.checkpoint_seq == 0U) {
        return durability::BatchCheck::corrupt;
    }
    const auto restore =
        model.tasks.restore_checkpoint(cursor.index, record.writer_id, record.writer_version,
                                       record.state, {0, task.checkpoint_seq});
    if (record.provenance_required &&
        !model.provenance.expect(work::ProvenanceKind::task, task.slot, task.task_id)) {
        return durability::BatchCheck::no_room;
    }
    cursor.checkpoint_seen = restore == work::TaskTable::Restore::restored;
    return snapshot_restore_check(restore);
}
// Final state must agree with the exact steps/checkpoint already restored.
// It then requires an adjacent authority record before another object can start.
durability::BatchCheck task_state(WorkModel &model, const durability::SnapshotMetadata &,
                                  StoreReadCursor &cursor, const uint8_t *body, uint32_t size,
                                  uint16_t) {
    work::TaskStateRecord record{};
    if (cursor.index == work::TaskTable::no_index ||
        !work::read_task_state_record(body, size, record)) {
        return durability::BatchCheck::corrupt;
    }
    const work::Task &task = model.tasks.at(cursor.index);
    if (record.change != work::TaskRecordChange::state || record.task_id != task.task_id ||
        task.steps_before > task.step_count ||
        (task.steps_seq == 0U) != (task.step_count == 0U) ||
        (task.checkpoint_seq != 0U) != cursor.checkpoint_seen) {
        return durability::BatchCheck::corrupt;
    }
    const auto restore = model.tasks.restore_state(cursor.index, record.state,
                                                   {task.created_transition_id, task.commit_seq});
    cursor.expected_type = work::task_authority_record_type;
    return snapshot_restore_check(restore);
}
// Live Tasks retain their exact Goal ceiling; terminal Tasks may preserve
// an earlier proven generation after explicit replacement (ADR-0038).
// Closing the admission releases only this snapshot read frontier.
durability::BatchCheck task_authority(WorkModel &model,
                                      const durability::SnapshotMetadata &metadata,
                                      StoreReadCursor &cursor, const uint8_t *body, uint32_t size) {
    work::TaskAuthorityRecord record{};
    if (cursor.index == work::TaskTable::no_index ||
        cursor.expected_type != work::task_authority_record_type ||
        !work::read_task_authority_record(body, size, record) ||
        !within_snapshot(record.stamp, metadata)) {
        return durability::BatchCheck::corrupt;
    }
    const auto &task = model.tasks.at(cursor.index);
    if (record.task_id != task.task_id || record.ceiling.goal_id != task.goal_id ||
        record.stamp.commit_seq > task.commit_seq ||
        record.stamp.transition_id < task.created_transition_id) {
        return durability::BatchCheck::corrupt;
    }
    const auto status = model.task_authority.install_validated(task.slot, record);
    if (status != AYNI_OK) {
        return status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                                : durability::BatchCheck::corrupt;
    }
    cursor = {};
    return durability::BatchCheck::ok;
}
durability::BatchCheck read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                            uint16_t version, const uint8_t *body, uint32_t size, uint16_t type,
                            StoreReadCursor &cursor) {
    switch (type) {
    case work::task_record_type:
        return task(model, metadata, cursor, version, body, size);
    case work::task_steps_record_type:
        return task_steps(model, metadata, cursor, body, size);
    case work::task_checkpoint_record_type:
        return task_checkpoint(model, metadata, cursor, version, body, size);
    case work::task_state_record_type:
        return task_state(model, metadata, cursor, body, size, version);
    case work::task_authority_record_type:
        return task_authority(model, metadata, cursor, body, size);
    default:
        return durability::BatchCheck::unsupported;
    }
}
const StoreKind kinds[] = {
    {work::task_record_type},
    {work::task_state_record_type},
    {work::task_steps_record_type},
    {work::task_checkpoint_record_type},
    {work::task_authority_record_type}};

} // namespace
const PersistentStore task_store{work::task_record_type,
                                 kinds,
                                 static_cast<uint32_t>(sizeof(kinds) / sizeof(kinds[0])),
                                 knows,
                                 StorePosition::tasks,
                                 capacity,
                                 record_max,
                                 journal_max,
                                 journal,
                                 next,
                                 shape,
                                 write,
                                 advance,
                                 read,
                                 payload_max};
} // namespace ayni::kernel
