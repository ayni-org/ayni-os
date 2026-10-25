#include "authority/scene/scene_records.h"
#include "authority/scene/scene_definition.h"
#include "durability/batch/record_body.h"
#include "foundation/kernel_id.h"
namespace ayni::authority {
uint32_t scene_record_size(const SceneState &state) {
    return scene_record_fixed_bytes + state.scene.size() + state.definition_size;
}
uint32_t scene_record_max(uint32_t scope_bytes) {
    return scene_record_fixed_bytes + catalog::CatalogId::max_size +
           Scenes::definition_max_bytes(scope_bytes);
}
void write_scene_record(const SceneState &state, ayni_bytes_v1 definition, uint8_t *body) {
    durability::BodyWriter writer(body, scene_record_size(state));
    writer.u64(state.object_id);
    writer.bytes(state.scene.data(), state.scene.size());
    writer.u32(state.definition_version);
    writer.u32(state.revision);
    writer.u32(static_cast<uint32_t>(state.mode));
    writer.u64(state.stamp.transition_id);
    writer.u64(state.stamp.commit_seq);
    writer.bytes(definition.data, definition.size);
}
// Representation and semantic validation are independent of this boot's current
// declaration. Compatibility is checked separately when enabling or consuming.
bool read_scene_record(const uint8_t *body, uint32_t size, SceneRecord &out) {
    if (body == nullptr || size < scene_record_fixed_bytes) {
        return false;
    }
    durability::BodyReader reader(body, size);
    SceneRecord record{};
    auto &state = record.state;
    state.object_id = reader.u64();
    uint32_t id_size = 0;
    const auto *id = reader.bytes(catalog::CatalogId::max_size, id_size);
    if (!state.scene.assign({reinterpret_cast<const char *>(id), id_size})) {
        return false;
    }
    state.definition_version = reader.u32();
    state.revision = reader.u32();
    state.mode = static_cast<SceneMode>(reader.u32());
    state.stamp = {reader.u64(), reader.u64()};
    record.definition.data = reader.bytes(
        Scenes::definition_max_bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX), state.definition_size);
    record.definition.size = state.definition_size;
    SceneDefinition definition{};
    if (!reader.ok() || !foundation::is_kernel_id(state.object_id) ||
        state.definition_version == 0 || state.revision == 0 || state.stamp.transition_id == 0 ||
        state.stamp.commit_seq == 0 ||
        (state.mode != SceneMode::disabled && state.mode != SceneMode::enabled) ||
        !read_scene_definition(record.definition, definition)) {
        return false;
    }
    out = record;
    return true;
}
} // namespace ayni::authority
