// Scene journal pairing and snapshot bounds share authority's one original-fact codec.
#include "authority/scene/scene_records.h"
#include "foundation/kernel_id.h"
#include "kernel/work_replay.h"
#include "kernel/work_snapshot.h"
#include <cstring>

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
bool same_bytes(ayni_bytes_v1 left, ayni_bytes_v1 right) {
    return left.size == right.size && std::memcmp(left.data, right.data, left.size) == 0;
}
} // namespace
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
durability::BatchCheck WorkSnapshotReader::scene(const uint8_t *body, uint32_t size) {
    using durability::BatchCheck;
    authority::SceneRecord record{};
    const auto *binding = model_.principals.binding();
    uint32_t duplicate = 0;
    if (!authority::read_scene_record(body, size, record) || binding == nullptr ||
        record.state.stamp.transition_id > metadata_.max_transition_id ||
        record.state.stamp.commit_seq > metadata_.covers_commit_seq ||
        record.state.stamp.transition_id <= binding->stamp.transition_id ||
        record.state.stamp.commit_seq <= binding->stamp.commit_seq ||
        foundation::id_boot_epoch(record.state.object_id) > metadata_.boot_epoch ||
        model_.scenes.find_state(view(record.state.scene), duplicate)) {
        return BatchCheck::corrupt;
    }
    const auto status = model_.scenes.restore_validated(record.state, record.definition);
    return status == AYNI_OK                  ? BatchCheck::ok
           : status == AYNI_CAPACITY_EXCEEDED ? BatchCheck::no_room
                                              : BatchCheck::corrupt;
}
} // namespace ayni::kernel
