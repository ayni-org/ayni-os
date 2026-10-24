#include "kernel/command/command_approval.h"

#include "authority/grant/grant_rules.h"
#include "durability/batch/record_body.h"

#include <cstring>

namespace ayni::kernel {
namespace {

// The configured count may be smaller, but request syntax uses the ABI ceiling.
constexpr uint32_t requirement_mask = (uint32_t{1} << AYNI_GOAL_REQUIREMENT_CAPACITY_MAX) - 1U;

bool same_bytes(ayni_bytes_v1 left, ayni_bytes_v1 right) {
    return left.size == right.size &&
           (left.size == 0U || std::memcmp(left.data, right.data, left.size) == 0);
}

ayni_string_v1 read_id(durability::BodyReader &reader) {
    uint32_t size = 0;
    const auto *bytes = reader.bytes(AYNI_CATALOG_ID_MAX_BYTES, size);
    return {reinterpret_cast<const char *>(bytes), size};
}

} // namespace

ayni_status_t measure_approval_command(const ApprovalCommand &input, uint32_t &out_size) {
    const auto &approval = input.approval;
    const auto &create = approval.original;
    if (input.expected_revision == 0U || !foundation::is_kernel_id(approval.goal_id) ||
        approval.missing_mask == 0U || (approval.missing_mask & ~requirement_mask) != 0U ||
        !authority::valid_grant_lifetime(approval.lifetime, approval.duration_us) ||
        !foundation::is_kernel_id(create.owner_instance_id) || create.goal_type_version == 0U ||
        (create.parent_goal_id != 0U && !foundation::is_kernel_id(create.parent_goal_id)) ||
        create.parameters.size > AYNI_GOAL_PARAMETER_MAX_BYTES_MAX ||
        check_goal_form(create) != AYNI_OK) {
        return AYNI_INVALID_ARGUMENT;
    }
    out_size = approval_command_fixed_size + create.goal_type_id.size + create.trigger_source.size +
               create.parameters.size;
    return AYNI_OK;
}

// All fallible shape/buffer checks precede the single complete writer. The
// canonical identity retains original durations, never a live deadline.
ayni_status_t encode_approval_command(const ApprovalCommand &input, uint8_t *buffer,
                                      uint32_t capacity, uint32_t &out_size) {
    uint32_t needed = 0;
    const auto status = measure_approval_command(input, needed);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < needed) {
        out_size = needed;
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (buffer == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &approval = input.approval;
    const auto &create = approval.original;
    durability::BodyWriter writer(buffer, needed);
    writer.u32(AYNI_INTENT_GOAL_APPROVE);
    writer.u32(input.expected_revision);
    writer.u64(approval.goal_id);
    writer.u32(approval.missing_mask);
    writer.u32(approval.lifetime);
    writer.u64(approval.duration_us);
    writer.u64(create.owner_instance_id);
    writer.u64(create.parent_goal_id);
    writer.u32(create.goal_type_version);
    writer.bytes(create.goal_type_id.data, create.goal_type_id.size);
    writer.bytes(create.parameters.data, create.parameters.size);
    writer.u32(create.trigger.kind);
    writer.u64(create.trigger.delay_us);
    writer.u64(create.trigger.period_us);
    writer.u64(create.expiry_us);
    writer.bytes(create.trigger_source.data, create.trigger_source.size);
    if (!writer.complete()) {
        return AYNI_INTERNAL;
    }
    out_size = needed;
    return AYNI_OK;
}

// Decode into borrowed temporaries and reuse the same request-shape rule.
// Catalog drift or reclaiming the Goal cannot change this identity's meaning.
bool read_approval_command(ayni_bytes_v1 bytes, ApprovalCommand &out) {
    if (bytes.data == nullptr || bytes.size < approval_command_fixed_size ||
        bytes.size > approval_command_max_bytes(AYNI_GOAL_PARAMETER_MAX_BYTES_MAX)) {
        return false;
    }
    durability::BodyReader reader(bytes.data, bytes.size);
    if (reader.u32() != AYNI_INTENT_GOAL_APPROVE) {
        return false;
    }
    ApprovalCommand input{};
    input.expected_revision = reader.u32();
    auto &approval = input.approval;
    auto &create = approval.original;
    approval.goal_id = reader.u64();
    approval.missing_mask = reader.u32();
    approval.lifetime = reader.u32();
    approval.duration_us = reader.u64();
    create.owner_instance_id = reader.u64();
    create.parent_goal_id = reader.u64();
    create.goal_type_version = reader.u32();
    create.goal_type_id = read_id(reader);
    uint32_t parameter_size = 0;
    const auto *parameters = reader.bytes(AYNI_GOAL_PARAMETER_MAX_BYTES_MAX, parameter_size);
    create.parameters = {parameters, parameter_size};
    create.trigger.kind = reader.u32();
    create.trigger.delay_us = reader.u64();
    create.trigger.period_us = reader.u64();
    create.expiry_us = reader.u64();
    create.trigger_source = read_id(reader);
    uint32_t needed = 0;
    if (!reader.ok() || measure_approval_command(input, needed) != AYNI_OK ||
        needed != bytes.size) {
        return false;
    }
    out = input;
    return true;
}

bool matches_goal_creation(const GoalCreate &create, const work::Goal &goal,
                           ayni_bytes_v1 parameters) {
    return goal.owner_instance_id == create.owner_instance_id &&
           goal.parent_goal_id == create.parent_goal_id &&
           goal.goal_type_version == create.goal_type_version &&
           goal.goal_type_id.equals(create.goal_type_id) &&
           same_bytes(parameters, create.parameters) && goal.trigger.kind == create.trigger.kind &&
           goal.trigger.delay_us == create.trigger.delay_us &&
           goal.trigger.period_us == create.trigger.period_us &&
           goal.expiry_after_us == create.expiry_us &&
           goal.trigger_source.equals(create.trigger_source);
}

} // namespace ayni::kernel
