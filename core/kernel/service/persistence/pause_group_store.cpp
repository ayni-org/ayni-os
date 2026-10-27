// Group facts follow all work changes and precede the exact command that closes them.
#include "foundation/kernel_id.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"
#include "work/goal/pause_group_records.h"
namespace ayni::kernel {
namespace {
// Shared retained-fact boundary: group preferences require the already durable owner.
bool valid_history(const WorkModel &model, const work::PauseGroup &row, uint64_t boot_limit) {
    const auto *binding = model.principals.binding();
    return binding != nullptr && row.input.incarnation <= boot_limit &&
           foundation::id_boot_epoch(row.object_id) <= boot_limit &&
           row.stamp.commit_seq > binding->stamp.commit_seq &&
           row.stamp.transition_id > binding->stamp.transition_id;
}
bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == work::pause_group_record_version;
}
uint32_t capacity(const KernelCapacities &) {
    return AYNI_PAUSE_GROUP_COUNT;
}
uint32_t record_max(const KernelCapacities &) {
    return work::pause_group_record_size;
}
uint32_t journal_max(const KernelCapacities &) {
    return record_bytes(work::pause_group_record_size);
}
// Exactly one owner control changes a group in one transition; settlement may change many Tasks.
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    for (uint32_t group = 1; group <= AYNI_PAUSE_GROUP_COUNT; ++group) {
        const auto &row = model.pause_groups.at(group);
        if (row.stamp.commit_seq != stamp.commit_seq) {
            continue;
        }
        auto *body =
            writer.add_record_body({work::pause_group_record_type, work::pause_group_record_version,
                                    durability::record_required, work::pause_group_record_size});
        if (body == nullptr) {
            return false;
        }
        work::write_pause_group_record(group, row, body);
    }
    return true;
}
bool next(const WorkModel &model, StoreCursor &cursor) {
    while (cursor.index < AYNI_PAUSE_GROUP_COUNT &&
           model.pause_groups.at(cursor.index + 1U).revision == 0) {
        ++cursor.index;
    }
    return cursor.index < AYNI_PAUSE_GROUP_COUNT;
}
StoreRecord shape(const WorkModel &, const StoreCursor &) {
    return {work::pause_group_record_version, work::pause_group_record_size};
}
void write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto group = cursor.index + 1U;
    work::write_pause_group_record(group, model.pause_groups.at(group), body);
}
durability::BatchCheck read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                            uint16_t, const uint8_t *body, uint32_t size, uint16_t,
                            StoreReadCursor &) {
    work::PauseGroupRecord row{};
    return work::read_pause_group_record(body, size, row) &&
                   within_snapshot(row.state.stamp, metadata) &&
                   valid_history(model, row.state, metadata.boot_epoch) &&
                   model.pause_groups.restore_snapshot_validated(row.group, row.state)
               ? durability::BatchCheck::ok
               : durability::BatchCheck::corrupt;
}
const StoreKind kinds[] = {{work::pause_group_record_type, always_required}};
} // namespace
const PersistentStore pause_group_store{work::pause_group_record_type,
                                        kinds,
                                        1,
                                        knows,
                                        StorePosition::after_work,
                                        capacity,
                                        record_max,
                                        journal_max,
                                        journal,
                                        next,
                                        shape,
                                        write,
                                        advance_store_index,
                                        read};
// A control is always paired with an exact owner command; no preference may be forged
// by removing its acceptance proof from an otherwise framed journal batch.
durability::RecordUse WorkReplay::restore_pause_group(const uint8_t *body, uint32_t size,
                                                      foundation::TransitionStamp stamp,
                                                      uint64_t boot_limit) {
    work::PauseGroupRecord row{};
    if (!pairing_.complete() || pairing_.pause_group ||
        !work::read_pause_group_record(body, size, row) ||
        row.state.stamp.commit_seq != stamp.commit_seq ||
        row.state.stamp.transition_id != stamp.transition_id ||
        !valid_history(model_, row.state, boot_limit) ||
        (model_.pause_groups.at(row.group).object_id == 0 &&
         foundation::id_boot_epoch(row.state.object_id) != boot_limit) ||
        !model_.pause_groups.restore_journal_validated(row.group, row.state)) {
        return durability::RecordUse::corrupt;
    }
    pairing_.pause_group = BatchPairing::PauseGroupPair{row.group, false};
    return durability::RecordUse::applied;
}
// Historical receipts can precede a newer group preference. The same sequence must
// agree exactly; neither snapshot restoration nor retries re-run the control.
bool WorkReplay::matches_pause_group_command(const CommandMapping &mapping,
                                             ayni_bytes_v1 bytes) const {
    work::PauseGroupChange change{};
    const auto *binding = model_.principals.binding();
    if (!read_pause_group_command(bytes, change) || binding == nullptr ||
        mapping.principal_id != binding->principal_id ||
        mapping.commit_seq <= binding->stamp.commit_seq) {
        return false;
    }
    const auto &row = model_.pause_groups.at(change.group);
    return row.object_id == mapping.object.id && row.revision >= mapping.object.revision &&
           row.stamp.commit_seq >= mapping.commit_seq &&
           (row.stamp.commit_seq != mapping.commit_seq ||
            (row.revision == mapping.object.revision && row.paused == change.paused));
}
bool WorkReplay::pair_pause_group_command(const CommandMapping &mapping, ayni_bytes_v1 bytes) {
    work::PauseGroupChange change{};
    if (!pairing_.pause_group || pairing_.pause_group->command_seen ||
        !read_pause_group_command(bytes, change) || change.group != pairing_.pause_group->group ||
        !matches_pause_group_command(mapping, bytes)) {
        return false;
    }
    pairing_.pause_group->command_seen = true;
    return true;
}
// Group IDs are globally allocated and cannot alias any other retained entity.
bool WorkReplay::pause_groups_consistent(uint64_t boot_epoch_limit) const {
    for (uint32_t group = 1; group <= AYNI_PAUSE_GROUP_COUNT; ++group) {
        const auto &row = model_.pause_groups.at(group);
        if (row.object_id == 0) {
            continue;
        }
        if (!valid_history(model_, row, boot_epoch_limit)) {
            return false;
        }
        uint32_t index = 0;
        if (model_.memories.find(row.object_id, index) ||
            model_.contexts.find(row.object_id, index) || model_.goals.find(row.object_id, index) ||
            model_.tasks.find(row.object_id, index) || model_.grants.find(row.object_id, index) ||
            model_.calls.find(row.object_id, index) ||
            model_.agents.find_instance(row.object_id) != nullptr) {
            return false;
        }
        for (uint32_t other = 1; other < group; ++other) {
            if (model_.pause_groups.at(other).object_id == row.object_id) {
                return false;
            }
        }
        for (uint32_t at = 0; at < model_.scenes.state_count(); ++at) {
            if (model_.scenes.state(at).object_id == row.object_id) {
                return false;
            }
        }
    }
    return true;
}
} // namespace ayni::kernel
