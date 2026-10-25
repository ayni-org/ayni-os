#include "authority/scene/scene_table.h"
#include "authority/scene/scene_definition.h"
#include "authority/scope/scope.h"
#include "catalog/abi_checks.h"
#include <cstring>

namespace ayni::authority {
namespace {
constexpr uint32_t initial_revision = 1;
constexpr uint32_t definition_pools = 2;
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
bool equal_bytes(ayni_bytes_v1 left, ayni_bytes_v1 right) {
    return left.size == right.size &&
           (left.size == 0 || std::memcmp(left.data, right.data, left.size) == 0);
}
std::size_t metadata_size(uint32_t capacity) {
    return foundation::BoundedArray<SceneDefinition>::storage_size(capacity) +
           foundation::BoundedArray<SceneState>::storage_size(capacity);
}
uint8_t *offset(void *storage, std::size_t bytes) {
    return storage == nullptr ? nullptr : static_cast<uint8_t *>(storage) + bytes;
}
} // namespace
ayni_status_t validate_scene_change(const SceneChange &change) {
    return change.scene.size() > 0 &&
                   ((change.mode == SceneMode::enabled && change.definition_version > 0) ||
                    (change.mode == SceneMode::disabled && change.definition_version == 0))
               ? AYNI_OK
               : AYNI_INVALID_ARGUMENT;
}
uint32_t Scenes::definition_max_bytes(uint32_t scope_bytes) {
    return scene_definition_header_bytes +
           AYNI_SCENE_RULE_COUNT_MAX * (scene_rule_fixed_bytes + catalog::CatalogId::max_size +
                                        definition_pools * scope_bytes) +
           AYNI_OBSERVATION_CONDITION_COUNT_MAX *
               (scene_condition_fixed_bytes + definition_pools * catalog::CatalogId::max_size);
}
std::size_t Scenes::storage_size(Limits limits) {
    return limits.capacity == 0 ? 0
                                : metadata_size(limits.capacity) +
                                      static_cast<std::size_t>(definition_pools) * limits.capacity *
                                          definition_max_bytes(limits.scope_bytes) +
                                      scene_definition_scratch_bytes(limits.scope_bytes);
}
Scenes::Scenes(void *storage, Limits limits, const catalog::CapabilityRegistry &capabilities,
               const catalog::ObservationSources &sources)
    : limits_(limits), definition_stride_(definition_max_bytes(limits.scope_bytes)),
      capabilities_(capabilities), sources_(sources), definitions_(storage, limits.capacity),
      states_(
          offset(storage, foundation::BoundedArray<SceneDefinition>::storage_size(limits.capacity)),
          limits.capacity),
      definitions_bytes_(offset(storage, metadata_size(limits.capacity))),
      states_bytes_(offset(definitions_bytes_,
                           static_cast<std::size_t>(limits.capacity) * definition_stride_)),
      scratch_(
          offset(states_bytes_, static_cast<std::size_t>(limits.capacity) * definition_stride_)) {}
uint8_t *Scenes::definition_bytes(uint32_t index) const {
    return definitions_bytes_ + static_cast<std::size_t>(index) * definition_stride_;
}
uint8_t *Scenes::state_bytes(uint32_t index) const {
    return states_bytes_ + static_cast<std::size_t>(index) * definition_stride_;
}
ayni_bytes_v1 Scenes::definition_view(uint32_t index) const {
    return {definition_bytes(index), definitions_[index].bytes};
}
ayni_bytes_v1 Scenes::state_definition(uint32_t index) const {
    return {state_bytes(index), states_[index].definition_size};
}
bool Scenes::find_definition(ayni_string_v1 id, uint32_t &index) const {
    for (uint32_t at = 0; at < definitions_.size(); ++at) {
        if (definitions_[at].id.equals(id)) {
            index = at;
            return true;
        }
    }
    return false;
}
bool Scenes::find_state(ayni_string_v1 id, uint32_t &index) const {
    for (uint32_t at = 0; at < states_.size(); ++at) {
        if (states_[at].scene.equals(id)) {
            index = at;
            return true;
        }
    }
    return false;
}
// Admission is all-or-nothing: normalized Scopes and source metadata are staged
// before either the immutable metadata or the canonical declaration is published.
ayni_status_t Scenes::register_description(const ayni_scene_desc_v1 &input) {
    const auto shape = catalog::check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (shape != AYNI_OK) {
        return shape;
    }
    uint32_t duplicate = 0;
    if (!catalog::CatalogId::is_valid(input.scene_id) || input.version == 0 ||
        input.rule_count == 0 || input.rule_count > AYNI_SCENE_RULE_COUNT_MAX ||
        input.rules == nullptr || input.condition_count > AYNI_OBSERVATION_CONDITION_COUNT_MAX ||
        (input.condition_count > 0 && input.conditions == nullptr) ||
        find_definition(input.scene_id, duplicate)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (definitions_.full()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    SceneDefinition definition{};
    ayni_bytes_v1 bytes{};
    const auto status = build_scene_definition(input, capabilities_, sources_, limits_.scope_bytes,
                                               scratch_, definition, bytes);
    if (status != AYNI_OK) {
        return status;
    }
    std::memcpy(definition_bytes(definitions_.size()), bytes.data, bytes.size);
    return definitions_.push_back(definition) ? AYNI_OK : AYNI_INTERNAL;
}
bool Scenes::matches(uint32_t definition_index, uint32_t state_index) const {
    return definitions_[definition_index].version == states_[state_index].definition_version &&
           equal_bytes(definition_view(definition_index), state_definition(state_index));
}
// A disabled orphan remains manageable after a configuration change. Enabling
// always acknowledges the current complete definition, including original types.
ayni_status_t Scenes::prepare(const SceneChange &change, PreparedSceneChange &out) const {
    PreparedSceneChange prepared{no_index, no_index, initial_revision, true};
    find_definition(view(change.scene), prepared.definition_index);
    find_state(view(change.scene), prepared.state_index);
    if (prepared.definition_index == no_index &&
        (change.mode == SceneMode::enabled || prepared.state_index == no_index)) {
        return AYNI_NOT_FOUND;
    }
    if (change.mode == SceneMode::enabled &&
        change.definition_version != definitions_[prepared.definition_index].version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (prepared.state_index == no_index) {
        if (change.expected_revision != 0) {
            return AYNI_REVISION_CONFLICT;
        }
        if (states_.full()) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    } else {
        const auto &state = states_[prepared.state_index];
        if (change.expected_revision != state.revision) {
            return AYNI_REVISION_CONFLICT;
        }
        prepared.changes_state = state.mode != change.mode ||
                                 (change.mode == SceneMode::enabled &&
                                  !matches(prepared.definition_index, prepared.state_index));
        if (prepared.changes_state && state.revision == UINT32_MAX) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        prepared.next_revision = state.revision + (prepared.changes_state ? 1U : 0U);
    }
    out = prepared;
    return AYNI_OK;
}
void Scenes::apply(const SceneChange &change, const PreparedSceneChange &prepared,
                   uint64_t object_id, foundation::TransitionStamp stamp) {
    if (!prepared.changes_state) {
        return;
    }
    const bool creating = prepared.state_index == no_index;
    const auto index = creating ? states_.size() : prepared.state_index;
    SceneState next = creating ? SceneState{} : states_[index];
    next.object_id = object_id;
    next.scene = change.scene;
    next.revision = prepared.next_revision;
    next.mode = change.mode;
    next.stamp = stamp;
    if (creating || change.mode == SceneMode::enabled) {
        const auto &definition = definitions_[prepared.definition_index];
        next.definition_version = definition.version;
        next.definition_size = definition.bytes;
        std::memcpy(state_bytes(index), definition_bytes(prepared.definition_index),
                    definition.bytes);
    }
    if (creating) {
        states_.push_back(next);
    } else {
        states_[index] = next;
    }
}
ayni_status_t Scenes::authorize(ayni_string_v1 id, uint32_t version, ayni_string_v1 capability,
                                uint32_t capability_version, ayni_bytes_v1 arguments,
                                uint64_t durable_seq, uint32_t &definition_index) const {
    uint32_t configured = 0;
    uint32_t saved = 0;
    if (!find_definition(id, configured) || !find_state(id, saved) ||
        states_[saved].mode != SceneMode::enabled) {
        return AYNI_DENIED;
    }
    if (definitions_[configured].version != version || !matches(configured, saved)) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (states_[saved].stamp.commit_seq > durable_seq) {
        return AYNI_BUSY;
    }
    const auto &definition = definitions_[configured];
    const auto *bytes = definition_bytes(configured);
    for (uint32_t at = 0; at < definition.rule_count; ++at) {
        const auto &rule = definition.rules[at];
        if (rule.capability.equals(capability) && rule.version == capability_version) {
            const auto coverage =
                contains_request({bytes + rule.domain_offset, rule.domain_size},
                                 {bytes + rule.scope_offset, rule.scope_size}, arguments);
            if (coverage.status != AYNI_OK) {
                return coverage.status;
            }
            if (!coverage.covered) {
                return AYNI_DENIED;
            }
            definition_index = configured;
            return AYNI_OK;
        }
    }
    return AYNI_DENIED;
}
ayni_status_t Scenes::info(ayni_string_v1 id, uint64_t durable_seq, ayni_scene_info_v1 &out) const {
    uint32_t configured = no_index;
    uint32_t saved = no_index;
    find_definition(id, configured);
    find_state(id, saved);
    if (configured == no_index && saved == no_index) {
        return AYNI_NOT_FOUND;
    }
    ayni_scene_info_v1 info{};
    info.struct_size = sizeof(info);
    if (configured != no_index) {
        const auto &definition = definitions_[configured];
        info.registered_version = definition.version;
        info.rule_count = definition.rule_count;
        info.condition_count = definition.condition_count;
    }
    if (saved != no_index) {
        const auto &state = states_[saved];
        info.object_id = state.object_id;
        info.revision = state.revision;
        info.enabled = state.mode == SceneMode::enabled ? 1U : 0U;
        info.saved_version = state.definition_version;
        info.requires_seq = state.stamp.commit_seq;
        info.consumable = info.enabled != 0 && configured != no_index &&
                                  matches(configured, saved) && info.requires_seq <= durable_seq
                              ? 1U
                              : 0U;
    }
    out = info;
    return AYNI_OK;
}
ayni_status_t Scenes::restore_validated(const SceneState &state, ayni_bytes_v1 definition) {
    if (definition.size > definition_stride_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    uint32_t index = no_index;
    find_state(view(state.scene), index);
    for (uint32_t at = 0; at < states_.size(); ++at) {
        if (at != index && states_[at].object_id == state.object_id) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    if (index == no_index) {
        if (states_.full()) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        index = states_.size();
        states_.push_back(state);
    } else {
        const auto &previous = states_[index];
        if (previous.object_id != state.object_id || previous.revision == UINT32_MAX ||
            state.revision != previous.revision + 1 ||
            state.stamp.transition_id <= previous.stamp.transition_id ||
            state.stamp.commit_seq <= previous.stamp.commit_seq) {
            return AYNI_INVALID_ARGUMENT;
        }
        states_[index] = state;
    }
    std::memcpy(state_bytes(index), definition.data, definition.size);
    return AYNI_OK;
}
} // namespace ayni::authority
