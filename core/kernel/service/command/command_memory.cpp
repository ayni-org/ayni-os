#include "kernel/service/command/command_memory.h"
#include "catalog/abi_checks.h"
#include "foundation/kernel_id.h"
namespace ayni::kernel {
// Management supplies explicit user content; source, confirmation and lineage cannot be claimed.
ayni_status_t decode_memory_command(const ayni_intent_v1 &raw, MemoryCommand &out) {
    MemoryCommand command{};
    command.kind = raw.kind;
    command.expected_revision = raw.expected_revision;
    if (raw.kind == AYNI_INTENT_MEMORY_SAVE) {
        const auto &body = *static_cast<const ayni_memory_save_v1 *>(raw.body);
        const auto shape = catalog::check_struct_size(body.struct_size, sizeof(body), sizeof(body));
        if (shape != AYNI_OK) {
            return shape;
        }
        if (body.reserved != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        command.target_id = body.memory_id;
        auto &v = command.value;
        v.space = static_cast<knowledge::MemoryNamespace>(body.name_space);
        v.namespace_id = body.namespace_id;
        v.profile_field = static_cast<knowledge::ProfileField>(body.profile_field);
        v.source = knowledge::MemorySource::user;
        v.confirmed = true;
        v.body = body.body;
        v.labels.tag_count = body.tag_count;
        v.labels.keyword_count = body.keyword_count;
        if (!knowledge::read_memory_labels(body.tags, body.tag_count, body.keywords,
                                           body.keyword_count, v.labels)) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else {
        const auto &body = *static_cast<const ayni_memory_target_v1 *>(raw.body);
        const auto shape = catalog::check_struct_size(body.struct_size, sizeof(body), sizeof(body));
        if (shape != AYNI_OK) {
            return shape;
        }
        if (body.reserved != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        command.target_id = body.memory_id;
    }
    out = command;
    return AYNI_OK;
}
ayni_status_t measure_memory_command(const MemoryCommand &c, uint32_t &size) {
    if (!is_memory_command(c.kind) ||
        (c.target_id == 0 ? c.kind != AYNI_INTENT_MEMORY_SAVE || c.expected_revision != 0
                          : !foundation::is_kernel_id(c.target_id) || c.expected_revision == 0)) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t result = memory_command_fixed_bytes;
    if (c.kind == AYNI_INTENT_MEMORY_SAVE) {
        const auto &v = c.value;
        if (v.source != knowledge::MemorySource::user || !v.confirmed || v.source_call_id != 0 ||
            v.dependencies.count != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto status = knowledge::check_memory_value(v, {knowledge::memory_capacity_max,
                                                              knowledge::memory_body_max,
                                                              knowledge::memory_dependency_max});
        if (status != AYNI_OK) {
            return status;
        }
        result += knowledge::memory_value_size(v);
    }
    size = result;
    return AYNI_OK;
}
ayni_status_t encode_memory_command(const MemoryCommand &c, uint8_t *out, uint32_t capacity,
                                    uint32_t &written) {
    uint32_t size = 0;
    const auto status = measure_memory_command(c, size);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < size) {
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    durability::BodyWriter writer(out, size);
    writer.u32(c.kind);
    writer.u32(c.expected_revision);
    writer.u64(c.target_id);
    if (c.kind == AYNI_INTENT_MEMORY_SAVE) {
        knowledge::write_memory_value(c.value, writer);
    }
    written = size;
    return writer.complete() ? AYNI_OK : AYNI_INTERNAL;
}
bool read_memory_command(ayni_bytes_v1 bytes, MemoryCommand &out) {
    if (bytes.data == nullptr || bytes.size < memory_command_fixed_bytes) {
        return false;
    }
    durability::BodyReader reader(bytes.data, bytes.size);
    MemoryCommand c{};
    c.kind = reader.u32();
    c.expected_revision = reader.u32();
    c.target_id = reader.u64();
    if (c.kind == AYNI_INTENT_MEMORY_SAVE && !knowledge::read_memory_value(reader, c.value)) {
        return false;
    }
    uint32_t expected = 0;
    if (!reader.ok() || measure_memory_command(c, expected) != AYNI_OK || expected != bytes.size) {
        return false;
    }
    out = c;
    return true;
}
} // namespace ayni::kernel
