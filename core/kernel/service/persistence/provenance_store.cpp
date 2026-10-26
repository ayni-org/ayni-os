// Work content provenance follows its Goal/Task records; representation belongs to work/provenance.
#include "foundation/kernel_id.h"
#include "kernel/service/memory/memory_permissions.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"
#include "work/provenance/provenance_records.h"
namespace ayni::kernel {
namespace {
bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == work::provenance_record_version;
}
uint32_t capacity(const KernelCapacities &c) {
    const auto limits = provenance_limits(c);
    return limits.goals + limits.tasks;
}
uint32_t record_max(const KernelCapacities &c) {
    return work::provenance_record_size(c.memory_dependency_capacity);
}
const StoreKind kinds[] = {{work::provenance_record_type, always_required}};
uint32_t journal_max(const KernelCapacities &c) {
    return capacity(c) == 0 ? 0 : record_bytes(record_max(c));
}
// Stable work slots give up their provenance together; only occupied exact identities are
// serialized.
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < model.provenance.capacity(); ++i) {
        const auto &r = model.provenance.at(i);
        if (r.id == 0 || r.stamp.commit_seq != stamp.commit_seq) {
            continue;
        }
        if (++count > 1) {
            return false;
        }
        auto *body = writer.add_record_body(
            {work::provenance_record_type, work::provenance_record_version,
             durability::record_required, work::provenance_record_size(r.dependencies.count)});
        if (body == nullptr) {
            return false;
        }
        work::write_provenance_record(r, body);
    }
    return true;
}
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    while (cursor.index < model.provenance.capacity() &&
           model.provenance.at(cursor.index).id == 0) {
        ++cursor.index;
    }
    return cursor.index < model.provenance.capacity();
}
StoreRecord snapshot_record(const WorkModel &model, uint32_t index) {
    return {work::provenance_record_version,
            work::provenance_record_size(model.provenance.at(index).dependencies.count)};
}
void snapshot_write(const WorkModel &model, uint32_t index, uint8_t *body) {
    work::write_provenance_record(model.provenance.at(index), body);
}
// Rebind only to its original work identity; snapshot compaction may choose different row indices.
bool work_slot(const WorkModel &model, const work::ProvenanceRecord &record, uint32_t &slot,
               uint64_t &latest) {
    uint32_t index = 0;
    if (record.kind == work::ProvenanceKind::goal && model.goals.find(record.id, index)) {
        slot = index;
        latest = model.goals.at(index).commit_seq;
        return true;
    }
    if (record.kind == work::ProvenanceKind::task && model.tasks.find(record.id, index)) {
        slot = model.tasks.at(index).slot;
        latest = model.tasks.at(index).checkpoint_seq;
        return true;
    }
    return false;
}
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t, const uint8_t *body, uint32_t size) {
    work::ProvenanceRecord record{};
    uint32_t slot = 0;
    uint64_t latest = 0;
    if (!work::read_provenance_record(body, size, record) ||
        !within_snapshot(record.stamp, metadata) || !work_slot(model, record, slot, latest) ||
        record.stamp.commit_seq > latest ||
        (record.kind == work::ProvenanceKind::task && record.stamp.commit_seq != latest)) {
        return durability::BatchCheck::corrupt;
    }
    // The earlier REQUIRED work record reserved this row; an unmarked or duplicate footer
    // cannot silently attach a source set to independent historical work.
    const auto *pending = model.provenance.find(record.kind, slot, record.id);
    if (pending == nullptr || pending->content != work::ContentState::awaiting_record) {
        return durability::BatchCheck::corrupt;
    }
    const auto status =
        model.provenance.restore(slot, record, model.memories.limits().dependencies);
    return status == AYNI_OK                  ? durability::BatchCheck::ok
           : status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                              : durability::BatchCheck::corrupt;
}
} // namespace
const PersistentStore provenance_store{work::provenance_record_type,
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

// A content-source change is part of that owner's already restored transition, never a
// free-standing mutation.
durability::RecordUse WorkReplay::restore_provenance(const uint8_t *body, uint32_t size,
                                                     foundation::TransitionStamp stamp) {
    work::ProvenanceRecord record{};
    uint32_t slot = 0;
    uint64_t latest = 0;
    if (!work::read_provenance_record(body, size, record) ||
        record.stamp.commit_seq != stamp.commit_seq ||
        record.stamp.transition_id != stamp.transition_id ||
        !work_slot(model_, record, slot, latest) || latest != stamp.commit_seq) {
        return durability::RecordUse::corrupt;
    }
    if (record.kind == work::ProvenanceKind::task &&
        (!pairing_.checkpoint_sources || *pairing_.checkpoint_sources != record.id)) {
        return durability::RecordUse::corrupt;
    }
    if (pairing_.memory && pairing_.memory->policy_task_id != 0) {
        uint32_t memory_index = 0;
        if (record.kind != work::ProvenanceKind::task ||
            record.id != pairing_.memory->policy_task_id ||
            !model_.memories.find(pairing_.memory->object_id, memory_index)) {
            return durability::RecordUse::corrupt;
        }
        const auto &sources = model_.memories.at(memory_index).dependencies;
        if (sources.count != record.dependencies.count ||
            !knowledge::contains_dependencies(sources, record.dependencies) ||
            !knowledge::contains_dependencies(record.dependencies, sources)) {
            return durability::RecordUse::corrupt;
        }
        pairing_.memory->command_seen = true;
    }
    if (record.kind == work::ProvenanceKind::task) {
        pairing_.checkpoint_sources.reset();
    }
    const auto status =
        model_.provenance.restore(slot, record, model_.memories.limits().dependencies);
    return status == AYNI_OK                  ? durability::RecordUse::applied
           : status == AYNI_CAPACITY_EXCEEDED ? durability::RecordUse::no_room
                                              : durability::RecordUse::corrupt;
}
// Every restored source row belongs to retained work and preserves its original actor/scope facts.
bool WorkReplay::provenance_consistent(uint64_t boot_epoch_limit) const {
    for (uint32_t i = 0; i < model_.provenance.capacity(); ++i) {
        const auto &record = model_.provenance.at(i);
        if (record.id == 0) {
            continue;
        }
        uint32_t index = 0;
        uint64_t actor = 0;
        uint64_t sequence = 0;
        if (record.kind == work::ProvenanceKind::task && model_.tasks.find(record.id, index)) {
            actor = model_.tasks.at(index).owner_instance_id;
            sequence = model_.tasks.at(index).checkpoint_seq;
        } else if (record.kind == work::ProvenanceKind::goal &&
                   model_.goals.find(record.id, index)) {
            actor = model_.goals.at(index).owner_instance_id;
            sequence = model_.goals.at(index).created.commit_seq;
        } else {
            return false;
        }
        if (record.stamp.commit_seq != sequence ||
            !memory_dependencies_history(model_, record.dependencies, boot_epoch_limit, actor)) {
            return false;
        }
    }
    return true;
}
} // namespace ayni::kernel
