#include "kernel/service/command/command_reauthorization.h"

#include "foundation/kernel_id.h"

namespace ayni::kernel {

ayni_status_t measure_reauthorization_command(const ReauthorizationCommand &input,
                                              uint32_t &out_size) {
    const auto &request = input.reauthorization;
    if (input.expected_revision == 0U || !foundation::is_kernel_id(request.goal_id) ||
        request.expected_authority_commit_seq == 0U ||
        !valid_goal_creation_identity(request.original)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out_size = reauthorization_command_fixed_size + request.original.goal_type_id.size +
               request.original.trigger_source.size + request.original.parameters.size +
               work::calendar_rule_size(request.original.calendar);
    return AYNI_OK;
}

// Exact identity is written only after the complete request fits. No clock,
// Goal lookup or ingress sample may change an accepted command's retry bytes.
ayni_status_t encode_reauthorization_command(const ReauthorizationCommand &input, uint8_t *body,
                                             uint32_t capacity, uint32_t &out_size) {
    uint32_t needed = 0;
    const auto status = measure_reauthorization_command(input, needed);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < needed) {
        out_size = needed;
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (body == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    durability::BodyWriter writer(body, needed);
    writer.u32(AYNI_INTENT_GOAL_REAUTHORIZE);
    writer.u32(input.expected_revision);
    writer.u64(input.reauthorization.goal_id);
    writer.u64(input.reauthorization.expected_authority_commit_seq);
    write_goal_creation_identity(input.reauthorization.original, writer);
    if (!writer.complete()) {
        return AYNI_INTERNAL;
    }
    out_size = needed;
    return AYNI_OK;
}

// The enclosing parser's reader and shared measure enforce exactly one raw
// request shape; rejected bytes never expose a partially decoded echo.
bool read_reauthorization_command(ayni_bytes_v1 bytes, ReauthorizationCommand &out) {
    if (bytes.data == nullptr || bytes.size < reauthorization_command_fixed_size ||
        bytes.size > reauthorization_command_max_bytes(AYNI_GOAL_PARAMETER_MAX_BYTES_MAX)) {
        return false;
    }
    durability::BodyReader reader(bytes.data, bytes.size);
    if (reader.u32() != AYNI_INTENT_GOAL_REAUTHORIZE) {
        return false;
    }
    ReauthorizationCommand input{};
    input.expected_revision = reader.u32();
    input.reauthorization.goal_id = reader.u64();
    input.reauthorization.expected_authority_commit_seq = reader.u64();
    input.reauthorization.original = read_goal_creation_identity(reader);
    uint32_t needed = 0;
    if (!reader.ok() || measure_reauthorization_command(input, needed) != AYNI_OK ||
        needed != bytes.size) {
        return false;
    }
    out = input;
    return true;
}
} // namespace ayni::kernel
