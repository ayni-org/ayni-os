#include "kernel/service/command/command_scene.h"
#include "foundation/record_body.h"
namespace ayni::kernel {
ayni_status_t measure_scene_command(const authority::SceneChange &input, uint32_t &size) {
    const auto status = authority::validate_scene_change(input);
    if (status == AYNI_OK) {
        size = scene_command_fixed_bytes + input.scene.size();
    }
    return status;
}
ayni_status_t encode_scene_command(const authority::SceneChange &input, uint8_t *out,
                                   uint32_t capacity, uint32_t &written) {
    uint32_t size = 0;
    const auto status = measure_scene_command(input, size);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < size) {
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    foundation::BodyWriter writer(out, size);
    writer.u32(AYNI_INTENT_SCENE_SET_ENABLED);
    writer.u32(input.expected_revision);
    writer.u32(static_cast<uint32_t>(input.mode));
    writer.u32(input.definition_version);
    writer.bytes(input.scene.data(), input.scene.size());
    written = size;
    return AYNI_OK;
}
bool read_scene_command(ayni_bytes_v1 bytes, authority::SceneChange &out) {
    if (bytes.data == nullptr || bytes.size < scene_command_fixed_bytes) {
        return false;
    }
    foundation::BodyReader reader(bytes.data, bytes.size);
    if (reader.u32() != AYNI_INTENT_SCENE_SET_ENABLED) {
        return false;
    }
    authority::SceneChange value{};
    value.expected_revision = reader.u32();
    value.mode = static_cast<authority::SceneMode>(reader.u32());
    value.definition_version = reader.u32();
    uint32_t size = 0;
    const auto *id = reader.bytes(catalog::CatalogId::max_size, size);
    if (!value.scene.assign({reinterpret_cast<const char *>(id), size}) || !reader.ok() ||
        authority::validate_scene_change(value) != AYNI_OK) {
        return false;
    }
    out = value;
    return true;
}
} // namespace ayni::kernel
