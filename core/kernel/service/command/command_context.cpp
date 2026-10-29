#include "kernel/service/command/command_context.h"
#include "catalog/abi_checks.h"
#include "foundation/kernel_id.h"
#include "foundation/record_body.h"
namespace ayni::kernel {
// Shape belongs here for both raw requests and persisted identities; live owner/state checks
// are performed only after this shared validator and exact retry lookup.
ayni_status_t measure_context_command(const ContextCommand &c, uint32_t &size) {
    using knowledge::ContextAction;
    if (!is_context_command(c.kind)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (c.kind == AYNI_INTENT_CONTEXT_OPEN) {
        if (!foundation::is_kernel_id(c.owner_instance_id) || c.expected_revision != 0 ||
            c.context_id != 0 || c.generation != 0 || c.task_id != 0 || c.turn_id != 0 ||
            c.action != ContextAction::none) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else {
        if (!foundation::is_kernel_id(c.context_id) || c.expected_revision == 0 ||
            c.owner_instance_id != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (c.kind == AYNI_INTENT_CONTEXT_PUBLISH) {
            if (!foundation::is_kernel_id(c.task_id) || c.turn_id == 0 ||
                c.action != ContextAction::none) {
                return AYNI_INVALID_ARGUMENT;
            }
        } else if (c.kind == AYNI_INTENT_CONTEXT_FORGET) {
            if (c.task_id != 0 ||
                ((c.action == ContextAction::erase_all || c.action == ContextAction::erase_derived)
                     ? c.turn_id != 0
                     : c.action != ContextAction::erase_round || c.turn_id == 0)) {
                return AYNI_INVALID_ARGUMENT;
            }
        } else if (c.task_id != 0 || c.turn_id != 0 || c.action != ContextAction::none) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    size = context_command_size;
    return AYNI_OK;
}
ayni_status_t decode_context_command(const ayni_intent_v1 &raw, ContextCommand &out) {
    const auto &body = *static_cast<const ayni_context_command_v1 *>(raw.body);
    const auto shape = catalog::check_struct_size(body.struct_size, sizeof(body), sizeof(body));
    if (shape != AYNI_OK) {
        return shape;
    }
    ContextCommand command{raw.kind,
                           raw.expected_revision,
                           body.owner_instance_id,
                           body.context_id,
                           body.expected_generation,
                           body.completed_task_id,
                           body.turn_id,
                           static_cast<knowledge::ContextAction>(body.action)};
    uint32_t size = 0;
    const auto valid = measure_context_command(command, size);
    if (valid == AYNI_OK) {
        out = command;
    }
    return valid;
}
ayni_status_t encode_context_command(const ContextCommand &c, uint8_t *out, uint32_t capacity,
                                     uint32_t &written) {
    uint32_t size = 0;
    const auto status = measure_context_command(c, size);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < size) {
        written = size;
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    foundation::BodyWriter writer(out, size);
    writer.u32(c.kind);
    writer.u32(c.expected_revision);
    writer.u64(c.owner_instance_id);
    writer.u64(c.context_id);
    writer.u64(c.generation);
    writer.u64(c.task_id);
    writer.u64(c.turn_id);
    writer.u32(static_cast<uint32_t>(c.action));
    writer.u32(0);
    written = size;
    return AYNI_OK;
}
bool read_context_command(ayni_bytes_v1 bytes, ContextCommand &out) {
    if (bytes.data == nullptr || bytes.size != context_command_size) {
        return false;
    }
    foundation::BodyReader reader(bytes.data, bytes.size);
    ContextCommand c{};
    c.kind = reader.u32();
    c.expected_revision = reader.u32();
    c.owner_instance_id = reader.u64();
    c.context_id = reader.u64();
    c.generation = reader.u64();
    c.task_id = reader.u64();
    c.turn_id = reader.u64();
    c.action = static_cast<knowledge::ContextAction>(reader.u32());
    const auto reserved = reader.u32();
    uint32_t size = 0;
    if (reserved != 0 || !reader.ok() || measure_context_command(c, size) != AYNI_OK) {
        return false;
    }
    out = c;
    return true;
}
} // namespace ayni::kernel
