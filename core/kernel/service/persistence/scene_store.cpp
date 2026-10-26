// The Scene store (persistent_store.h): journal, snapshot and replay share
// authority's one original-fact codec, and journal pairing with the owner's
// command stays in WorkReplay.
#include "authority/scene/scene_records.h"
#include "foundation/kernel_id.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"
#include <cstring>

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
bool same_bytes(ayni_bytes_v1 left, ayni_bytes_v1 right) {
    return left.size == right.size && std::memcmp(left.data, right.data, left.size) == 0;
}

// One body serves the log and the payload.
bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == authority::scene_record_version;
}
uint32_t capacity(const KernelCapacities &capacities) {
    return capacities.scene_capacity;
}
uint32_t record_max(const KernelCapacities &capacities) {
    return authority::scene_record_max(capacities.authority_scope_max_bytes);
}

const StoreKind kinds[] = {{authority::scene_record_type, always_required}};
uint32_t journal_max(const KernelCapacities &c) {
    return capacity(c) == 0U ? 0U : record_bytes(record_max(c));
}

// A Scene command changes one state; replay pairs it with that command.
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    uint32_t written = 0;
    for (uint32_t index = 0; index < model.scenes.state_count(); ++index) {
        const auto &scene = model.scenes.state(index);
        if (scene.stamp.commit_seq != stamp.commit_seq) {
            continue;
        }
        if (++written > 1U) {
            return false;
        }
        auto *at = writer.add_record_body(
            {authority::scene_record_type, authority::scene_record_version,
             durability::record_required, authority::scene_record_size(scene)});
        if (at == nullptr) {
            return false;
        }
        authority::write_scene_record(scene, model.scenes.state_definition(index), at);
    }
    return true;
}

// Every retained state is a row; the payload carries the journal body.
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    auto &index = cursor.index;
    return index < model.scenes.state_count();
}
StoreRecord snapshot_record(const WorkModel &model, uint32_t index) {
    return {authority::scene_record_version,
            authority::scene_record_size(model.scenes.state(index))};
}
void snapshot_write(const WorkModel &model, uint32_t index, uint8_t *body) {
    authority::write_scene_record(model.scenes.state(index), model.scenes.state_definition(index),
                                  body);
}

// A preference follows the binding that owns it and lies within the
// snapshot's boundary; its declaration is the one saved, not the catalog's.
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t, const uint8_t *body, uint32_t size) {
    using durability::BatchCheck;
    authority::SceneRecord record{};
    const auto *binding = model.principals.binding();
    uint32_t duplicate = 0;
    if (!authority::read_scene_record(body, size, record) || binding == nullptr ||
        !within_snapshot(record.state.stamp, metadata) ||
        record.state.stamp.transition_id <= binding->stamp.transition_id ||
        record.state.stamp.commit_seq <= binding->stamp.commit_seq ||
        foundation::id_boot_epoch(record.state.object_id) > metadata.boot_epoch ||
        model.scenes.find_state(view(record.state.scene), duplicate)) {
        return BatchCheck::corrupt;
    }
    const auto status = model.scenes.restore_validated(record.state, record.definition);
    return status == AYNI_OK                  ? BatchCheck::ok
           : status == AYNI_CAPACITY_EXCEEDED ? BatchCheck::no_room
                                              : BatchCheck::corrupt;
}
} // namespace

const PersistentStore scene_store{authority::scene_record_type,
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

// A changed row starts its own transition and must close with one exact owner
// command. Disable preserves the saved declaration; only enable can replace it.
durability::RecordUse WorkReplay::restore_scene(const uint8_t *body, uint32_t size,
                                                foundation::TransitionStamp stamp,
                                                uint64_t boot_epoch_limit) {
    using durability::RecordUse;
    authority::SceneRecord record{};
    const auto *binding = model_.principals.binding();
    if (batch_seq_ == stamp.commit_seq || !pairing_.complete() || binding == nullptr ||
        binding->stamp.commit_seq >= stamp.commit_seq ||
        binding->stamp.transition_id >= stamp.transition_id ||
        !authority::read_scene_record(body, size, record) ||
        record.state.stamp.transition_id != stamp.transition_id ||
        record.state.stamp.commit_seq != stamp.commit_seq ||
        foundation::id_boot_epoch(record.state.object_id) > boot_epoch_limit) {
        return RecordUse::corrupt;
    }
    uint32_t previous = 0;
    if (model_.scenes.find_state(view(record.state.scene), previous)) {
        const auto &old = model_.scenes.state(previous);
        if (record.state.mode == authority::SceneMode::disabled &&
            (old.mode == authority::SceneMode::disabled ||
             old.definition_version != record.state.definition_version ||
             !same_bytes(model_.scenes.state_definition(previous), record.definition))) {
            return RecordUse::corrupt;
        }
        if (record.state.mode == old.mode &&
            old.definition_version == record.state.definition_version &&
            same_bytes(model_.scenes.state_definition(previous), record.definition)) {
            return RecordUse::corrupt;
        }
    } else if (record.state.revision != 1 ||
               foundation::id_boot_epoch(record.state.object_id) != boot_epoch_limit) {
        return RecordUse::corrupt;
    }
    const auto status = model_.scenes.restore_validated(record.state, record.definition);
    if (status == AYNI_CAPACITY_EXCEEDED) {
        return RecordUse::no_room;
    }
    if (status != AYNI_OK) {
        return RecordUse::corrupt;
    }
    pairing_.scene = BatchPairing::ScenePair{record.state.object_id, false};
    return RecordUse::applied;
}
bool WorkReplay::pair_scene_command(const CommandMapping &mapping, ayni_bytes_v1 canonical) {
    if (!matches_scene_command(mapping, canonical) ||
        (mapping.effect == AYNI_INTENT_EFFECT_CHANGED
             ? !pairing_.scene || pairing_.scene->object_id != mapping.object.id
             : pairing_.scene.has_value() || batch_seq_ == mapping.commit_seq)) {
        return false;
    }
    pairing_.scene = BatchPairing::ScenePair{mapping.object.id, true};
    return true;
}
bool WorkReplay::matches_scene_command(const CommandMapping &mapping,
                                       ayni_bytes_v1 canonical) const {
    authority::SceneChange command{};
    const auto *binding = model_.principals.binding();
    uint32_t index = 0;
    if (!read_scene_command(canonical, command) || binding == nullptr ||
        mapping.principal_id != binding->principal_id ||
        mapping.commit_seq <= binding->stamp.commit_seq ||
        !model_.scenes.find_state(view(command.scene), index)) {
        return false;
    }
    const auto &state = model_.scenes.state(index);
    if (state.object_id != mapping.object.id || state.revision < mapping.object.revision) {
        return false;
    }
    if (state.revision > mapping.object.revision) {
        return state.stamp.commit_seq > mapping.commit_seq;
    }
    return state.mode == command.mode &&
           (command.mode != authority::SceneMode::enabled ||
            command.definition_version == state.definition_version) &&
           (mapping.effect == AYNI_INTENT_EFFECT_CHANGED
                ? state.stamp.commit_seq == mapping.commit_seq
                : state.stamp.commit_seq < mapping.commit_seq);
}
} // namespace ayni::kernel
