#include "kernel/service/command/command_pause_group.h"
#include "durability/batch/record_body.h"
namespace ayni::kernel {
ayni_status_t measure_pause_group_command(const work::PauseGroupChange &input, uint32_t &size) {
    if (!work::PauseGroups::valid_group(input.group)) {
        return AYNI_INVALID_ARGUMENT;
    }
    size = pause_group_command_size;
    return AYNI_OK;
}
ayni_status_t encode_pause_group_command(const work::PauseGroupChange &input, uint8_t *body,
                                         uint32_t capacity, uint32_t &written) {
    uint32_t size = 0;
    const auto status = measure_pause_group_command(input, size);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < size) {
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (body == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    durability::BodyWriter writer(body, size);
    writer.u32(AYNI_INTENT_PAUSE_GROUP_SET);
    writer.u32(input.expected_revision);
    writer.u32(input.group);
    writer.u32(input.paused ? 1U : 0U);
    written = size;
    return AYNI_OK;
}
bool read_pause_group_command(ayni_bytes_v1 bytes, work::PauseGroupChange &out) {
    if (bytes.data == nullptr || bytes.size != pause_group_command_size) {
        return false;
    }
    durability::BodyReader reader(bytes.data, bytes.size);
    if (reader.u32() != AYNI_INTENT_PAUSE_GROUP_SET) {
        return false;
    }
    work::PauseGroupChange value{};
    value.expected_revision = reader.u32();
    value.group = reader.u32();
    const auto paused = reader.u32();
    uint32_t size = 0;
    if (paused > 1 || !reader.ok() || measure_pause_group_command(value, size) != AYNI_OK) {
        return false;
    }
    value.paused = paused != 0;
    out = value;
    return true;
}
} // namespace ayni::kernel
