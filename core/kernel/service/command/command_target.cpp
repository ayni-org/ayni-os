#include "kernel/service/command/command_target.h"
#include "foundation/record_body.h"

namespace ayni::kernel {
ayni_status_t measure_target_command(const TargetCommand &input, uint32_t &size) {
    if (!is_target_command(input.kind) || input.expected_revision == 0U || input.target_id == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    size = target_command_size;
    return AYNI_OK;
}

ayni_status_t encode_target_command(const TargetCommand &input, uint8_t *out, uint32_t capacity,
                                    uint32_t &written) {
    uint32_t size = 0;
    const auto status = measure_target_command(input, size);
    if (status != AYNI_OK) {
        return status;
    }
    if (out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (capacity < size) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    foundation::BodyWriter writer(out, size);
    writer.u32(input.kind);
    writer.u32(input.expected_revision);
    writer.u64(input.target_id);
    written = size;
    return AYNI_OK;
}

bool read_target_command(ayni_bytes_v1 bytes, TargetCommand &out) {
    if (bytes.data == nullptr || bytes.size != target_command_size) {
        return false;
    }
    foundation::BodyReader reader(bytes.data, bytes.size);
    const auto kind = reader.u32();
    const TargetCommand input{kind, reader.u32(), reader.u64()};
    uint32_t size = 0;
    if (!reader.ok() || measure_target_command(input, size) != AYNI_OK) {
        return false;
    }
    out = input;
    return true;
}
} // namespace ayni::kernel
