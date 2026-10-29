// Recovery validates complete row identities before publishing any body to consumers.
// Memory owns its row codec; Kernel proves namespace identity and exact command pairing.
#include "foundation/kernel_id.h"
#include "kernel/service/memory/memory_access.h"
#include "kernel/service/memory/memory_permissions.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"
#include "knowledge/memory/memory_records.h"
namespace ayni::kernel {
namespace {
using namespace knowledge;
bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == memory_record_version;
}
uint32_t capacity(const KernelCapacities &c) {
    return c.memory_capacity;
}
uint32_t record_max(const KernelCapacities &c) {
    return memory_record_max(memory_limits(c));
}
uint32_t journal_max(const KernelCapacities &c) {
    return capacity(c) == 0 ? 0 : record_bytes(record_max(c));
}
const StoreKind kinds[] = {{memory_record_type}};
// A management or cleanup transition changes one memory row; no hidden bulk updates.
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < model.memories.size(); ++i) {
        const auto &record = model.memories.at(i);
        if (record.stamp.commit_seq != stamp.commit_seq || record.state == MemoryState::scrubbed) {
            continue;
        }
        if (++count > 1) {
            return false;
        }
        auto *body =
            writer.add_record_body({memory_record_type, memory_record_version,
                                    durability::record_required, memory_record_size(record)});
        if (body == nullptr) {
            return false;
        }
        write_memory_record(record, model.memories.body(i), body);
    }
    return true;
}
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    return cursor.index < model.memories.size();
}
StoreRecord snapshot_record(const WorkModel &model, const StoreCursor &cursor) {
    const auto index = cursor.index;
    return {memory_record_version, memory_record_size(model.memories.at(index))};
}
void snapshot_write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto index = cursor.index;
    write_memory_record(model.memories.at(index), model.memories.body(index), body);
}
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t, const uint8_t *body, uint32_t size, uint16_t,
                                     StoreReadCursor &) {
    DecodedMemory decoded{};
    uint32_t duplicate = 0;
    const auto *owner = model.principals.binding();
    if (!read_memory_record(body, size, decoded) || owner == nullptr ||
        !within_snapshot(decoded.record.stamp, metadata) ||
        decoded.record.stamp.commit_seq <= owner->stamp.commit_seq ||
        decoded.record.stamp.transition_id <= owner->stamp.transition_id ||
        foundation::id_boot_epoch(decoded.record.id) > metadata.boot_epoch ||
        model.memories.find(decoded.record.id, duplicate) ||
        !memory_namespace_exists(model, decoded.record.space, decoded.record.namespace_id)) {
        return durability::BatchCheck::corrupt;
    }
    const auto status = model.memories.restore(decoded.record, decoded.body);
    return status == AYNI_OK                  ? durability::BatchCheck::ok
           : status == AYNI_CAPACITY_EXCEEDED ? durability::BatchCheck::no_room
                                              : durability::BatchCheck::corrupt;
}
} // namespace
const PersistentStore memory_store{memory_record_type,
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

// Before-image rules prove CAS and deletion content preservation. Owner command closes the row.
durability::RecordUse WorkReplay::restore_memory(const uint8_t *body, uint32_t size,
                                                 foundation::TransitionStamp stamp,
                                                 uint64_t boot_epoch_limit) {
    using durability::RecordUse;
    DecodedMemory decoded{};
    const auto *owner = model_.principals.binding();
    const bool policy = pairing_.checkpoint_sources.has_value();
    if ((!policy && (batch_seq_ == stamp.commit_seq || !pairing_.complete())) || pairing_.memory ||
        owner == nullptr || owner->stamp.commit_seq >= stamp.commit_seq ||
        owner->stamp.transition_id >= stamp.transition_id ||
        !read_memory_record(body, size, decoded) ||
        decoded.record.stamp.commit_seq != stamp.commit_seq ||
        decoded.record.stamp.transition_id != stamp.transition_id ||
        foundation::id_boot_epoch(decoded.record.id) > boot_epoch_limit ||
        !memory_namespace_exists(model_, decoded.record.space, decoded.record.namespace_id)) {
        return RecordUse::corrupt;
    }
    const auto &record = decoded.record;
    // A policy mutation is never an owner command: its single new step, completed model origin
    // (when present), and matching source footer must all belong to this same Task transition.
    if (policy) {
        uint32_t task_index = 0;
        if (!model_.tasks.find(*pairing_.checkpoint_sources, task_index) ||
            model_.tasks.at(task_index).commit_seq != stamp.commit_seq ||
            model_.tasks.at(task_index).steps_seq != stamp.commit_seq ||
            model_.tasks.at(task_index).step_count - model_.tasks.at(task_index).steps_before !=
                1 ||
            record.state != MemoryState::live || record.confirmed ||
            record.space != MemoryNamespace::private_agent ||
            record.namespace_id != model_.tasks.at(task_index).owner_instance_id ||
            !memory_origin_matches(model_, *pairing_.checkpoint_sources, record.source,
                                   record.source_call_id, stamp.commit_seq - 1)) {
            return RecordUse::corrupt;
        }
    }
    uint32_t previous = 0;
    bool confirmation = false;
    bool revised = false;
    knowledge::MemoryDependencies released{};
    if (model_.memories.find(record.id, previous)) {
        revised = true;
        const auto &old = model_.memories.at(previous);
        if (old.state != MemoryState::live || old.revision == UINT32_MAX ||
            record.revision != old.revision + 1 || record.state == MemoryState::scrubbed) {
            return RecordUse::corrupt;
        }
        released = old.dependencies;
        auto before = memory_value(old, model_.memories.body(previous));
        const auto after = memory_value(record, decoded.body);
        if (record.state == MemoryState::tombstone && !same_memory_value(before, after)) {
            return RecordUse::corrupt;
        }
        before.confirmed = true;
        confirmation = record.state == MemoryState::live && same_memory_value(before, after);
    } else if (record.revision != memory_initial_revision || record.state != MemoryState::live ||
               foundation::id_boot_epoch(record.id) != boot_epoch_limit) {
        return RecordUse::corrupt;
    }
    const auto status = model_.memories.restore(record, decoded.body);
    if (status != AYNI_OK) {
        return status == AYNI_CAPACITY_EXCEEDED ? RecordUse::no_room : RecordUse::corrupt;
    }
    model_.release_content_references(released, stamp.commit_seq);
    pairing_.memory = BatchPairing::MemoryPair{
        record.id, confirmation, false, policy ? *pairing_.checkpoint_sources : 0, revised, false};
    return RecordUse::applied;
}
bool WorkReplay::pair_memory_command(const CommandMapping &mapping, ayni_bytes_v1 canonical) {
    if (!pairing_.memory || pairing_.memory->policy_task_id != 0 ||
        pairing_.memory->object_id != mapping.object.id ||
        !matches_memory_command(mapping, canonical) ||
        (mapping.kind == AYNI_INTENT_MEMORY_CONFIRM && !pairing_.memory->confirmation_only)) {
        return false;
    }
    pairing_.memory->command_seen = true;
    return true;
}
// Later versions supersede old request bytes. Equal-version facts must exactly match their receipt.
bool WorkReplay::matches_memory_command(const CommandMapping &mapping,
                                        ayni_bytes_v1 canonical) const {
    MemoryCommand command{};
    const auto *owner = model_.principals.binding();
    uint32_t index = 0;
    if (!read_memory_command(canonical, command) || owner == nullptr ||
        mapping.principal_id != owner->principal_id ||
        mapping.commit_seq <= owner->stamp.commit_seq ||
        !model_.memories.find(mapping.object.id, index)) {
        return false;
    }
    const auto &record = model_.memories.at(index);
    // Namespace and profile key never change across revisions; historical exact requests
    // must still name this identity even after their content has been superseded.
    if (command.kind == AYNI_INTENT_MEMORY_SAVE &&
        (command.value.space != record.space || command.value.namespace_id != record.namespace_id ||
         command.value.profile_field != record.profile_field)) {
        return false;
    }
    if (record.revision > mapping.object.revision) {
        return record.stamp.commit_seq > mapping.commit_seq;
    }
    if (record.revision != mapping.object.revision ||
        record.stamp.commit_seq != mapping.commit_seq) {
        return false;
    }
    if (command.kind == AYNI_INTENT_MEMORY_SAVE) {
        return record.state == MemoryState::live &&
               same_memory_value(command.value, memory_value(record, model_.memories.body(index)));
    }
    return command.kind == AYNI_INTENT_MEMORY_CONFIRM
               ? record.state == MemoryState::live && record.confirmed
               : record.state == MemoryState::tombstone;
}
// Journal and snapshots converge on this one cross-entity proof before publishing restored state.
bool WorkReplay::memory_identities_consistent(uint64_t boot_epoch_limit) const {
    for (uint32_t i = 0; i < model_.memories.size(); ++i) {
        const auto &record = model_.memories.at(i);
        uint32_t index = 0;
        if (foundation::id_boot_epoch(record.id) > boot_epoch_limit ||
            !memory_namespace_exists(model_, record.space, record.namespace_id) ||
            model_.goals.find(record.id, index) || model_.tasks.find(record.id, index) ||
            model_.grants.find(record.id, index) || model_.calls.find(record.id, index) ||
            model_.agents.find_instance(record.id) != nullptr ||
            foundation::id_boot_epoch(record.source_call_id) > boot_epoch_limit) {
            return false;
        }
        for (uint32_t scene = 0; scene < model_.scenes.state_count(); ++scene) {
            if (model_.scenes.state(scene).object_id == record.id) {
                return false;
            }
        }
        // Revoked/expired references are valid history, but their identity and original scope
        // remain exact. Current usability is decided separately at every consumption boundary.
        if (!memory_dependencies_history(model_, record.dependencies, boot_epoch_limit)) {
            return false;
        }
    }
    return true;
}
} // namespace ayni::kernel
