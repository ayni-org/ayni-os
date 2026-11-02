#include "kernel/service/command/command_task_limit.h"
#include "foundation/record_body.h"

namespace ayni::kernel {
ayni_status_t measure_task_limit_command(const TaskLimitCommand &input, uint32_t &size) {
    if (input.expected_revision == 0U || input.task_id == 0U || input.limit == 0U ||
        input.limit > AYNI_MAX_TURNS_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    size = task_limit_command_size;
    return AYNI_OK;
}

ayni_status_t encode_task_limit_command(const TaskLimitCommand &input, uint8_t *out,
                                        uint32_t capacity, uint32_t &written) {
    uint32_t size = 0;
    const auto status = measure_task_limit_command(input, size);
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
    writer.u32(AYNI_INTENT_TASK_RAISE_MODEL_LIMIT);
    writer.u32(input.expected_revision);
    writer.u64(input.task_id);
    writer.u32(input.limit);
    written = size;
    return AYNI_OK;
}

bool read_task_limit_command(ayni_bytes_v1 bytes, TaskLimitCommand &out) {
    if (bytes.data == nullptr || bytes.size != task_limit_command_size) {
        return false;
    }
    foundation::BodyReader reader(bytes.data, bytes.size);
    if (reader.u32() != AYNI_INTENT_TASK_RAISE_MODEL_LIMIT) {
        return false;
    }
    const TaskLimitCommand input{reader.u32(), reader.u64(), reader.u32()};
    uint32_t size = 0;
    if (!reader.ok() || measure_task_limit_command(input, size) != AYNI_OK) {
        return false;
    }
    out = input;
    return true;
}
} // namespace ayni::kernel
