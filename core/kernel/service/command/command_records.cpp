#include "kernel/service/command/command_records.h"

#include "durability/batch/record_body.h"

namespace ayni::kernel {
namespace {

void write_fixed(const CommandMapping &mapping, uint64_t floor, durability::BodyWriter &writer) {
    writer.u64(mapping.principal_id);
    writer.u64(mapping.command_id);
    writer.u64(mapping.digest);
    writer.u64(floor);
    writer.u32(mapping.kind);
    writer.u32(mapping.object.kind);
    writer.u64(mapping.object.id);
    writer.u32(mapping.object.revision);
}

CommandMapping read_fixed(durability::BodyReader &reader, uint64_t &floor) {
    CommandMapping mapping{};
    mapping.principal_id = reader.u64();
    mapping.command_id = reader.u64();
    mapping.digest = reader.u64();
    floor = reader.u64();
    mapping.kind = reader.u32();
    mapping.object.kind = reader.u32();
    mapping.object.id = reader.u64();
    mapping.object.revision = reader.u32();
    return mapping;
}

} // namespace

bool knows_command_record(uint16_t version) {
    return version == command_record_version || version == exact_command_record_version ||
           version == revoke_command_record_version ||
           version == work_authority_command_record_version ||
           version == task_limit_command_record_version ||
           version == call_query_command_record_version ||
           version == resume_wait_command_record_version ||
           version == scene_command_record_version || version == memory_command_record_version;
}

uint16_t command_record_version_for(const CommandMapping &mapping) {
    if (is_memory_command(mapping.kind)) {
        return memory_command_record_version;
    }
    if (mapping.kind == AYNI_INTENT_SCENE_SET_ENABLED) {
        return scene_command_record_version;
    }
    if (mapping.kind == AYNI_INTENT_TASK_RESUME_WAIT) {
        return resume_wait_command_record_version;
    }
    if (mapping.kind == AYNI_INTENT_CALL_QUERY_RESULT) {
        return call_query_command_record_version;
    }
    if (mapping.kind == AYNI_INTENT_TASK_RAISE_MODEL_LIMIT) {
        return task_limit_command_record_version;
    }
    if (is_work_authority_command(mapping.kind)) {
        return work_authority_command_record_version;
    }
    if (mapping.kind == AYNI_INTENT_GRANT_REVOKE) {
        return revoke_command_record_version;
    }
    return mapping.canonical_size == 0U ? command_record_version : exact_command_record_version;
}

uint32_t command_record_size_for(const CommandMapping &mapping) {
    const uint32_t maximum = is_memory_command(mapping.kind)
                                 ? memory_command_max(knowledge::memory_body_max)
                             : is_work_authority_command(mapping.kind)
                                 ? approval_command_max_bytes(AYNI_GOAL_PARAMETER_MAX_BYTES_MAX)
                                 : authority_command_max_bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX);
    if (mapping.canonical_size > maximum) {
        return 0;
    }
    if (is_work_authority_command(mapping.kind)) {
        return mapping.result_size >= work_authority_result_fixed_size &&
                       mapping.result_size <=
                           work_authority_result_size(AYNI_GOAL_REQUIREMENT_CAPACITY_MAX)
                   ? work_authority_command_record_fixed_size + mapping.canonical_size +
                         mapping.result_size
                   : 0U;
    }
    if (mapping.result_size != 0U) {
        return 0;
    }
    if (mapping.kind == AYNI_INTENT_SCENE_SET_ENABLED) {
        return revoke_command_record_fixed_size + mapping.canonical_size;
    }
    if (mapping.kind == AYNI_INTENT_GRANT_REVOKE) {
        return mapping.canonical_size == revoke_command_size
                   ? revoke_command_record_fixed_size + revoke_command_size
                   : 0U;
    }
    return mapping.canonical_size == 0U ? command_record_size
                                        : exact_command_record_fixed_size + mapping.canonical_size;
}

bool write_command_record(const CommandMapping &mapping, uint64_t floor, ayni_bytes_v1 canonical,
                          uint8_t *body, uint32_t size, ayni_bytes_v1 result) {
    if (body == nullptr || size == 0U || size != command_record_size_for(mapping) ||
        (mapping.canonical_size == 0U ? canonical.size != 0U || !is_ordinary_command(mapping.kind)
                                      : !valid_exact_mapping(mapping, canonical, result))) {
        return false;
    }
    durability::BodyWriter writer(body, size);
    write_fixed(mapping, floor, writer);
    if (mapping.canonical_size != 0U) {
        if (mapping.kind == AYNI_INTENT_GRANT_REVOKE ||
            mapping.kind == AYNI_INTENT_SCENE_SET_ENABLED) {
            writer.u32(mapping.effect);
        }
        writer.u32(authority_command_format);
        writer.bytes(canonical.data, canonical.size);
        if (mapping.result_size != 0U) {
            writer.bytes(result.data, result.size);
        }
    }
    return writer.complete();
}

void write_command_record(const CommandMapping &mapping, uint64_t floor, uint8_t *body) {
    static_cast<void>(write_command_record(mapping, floor, {}, body, command_record_size));
}

bool read_command_record(uint16_t version, const uint8_t *body, uint32_t size,
                         CommandMapping &out_mapping, uint64_t &out_floor,
                         ayni_bytes_v1 &out_canonical, ayni_bytes_v1 *out_result) {
    if (body == nullptr || !knows_command_record(version)) {
        return false;
    }
    durability::BodyReader reader(body, size);
    uint64_t floor = 0;
    CommandMapping mapping = read_fixed(reader, floor);
    ayni_bytes_v1 canonical{};
    ayni_bytes_v1 result{};
    if (version != command_record_version) {
        if (version == revoke_command_record_version || version == scene_command_record_version) {
            mapping.effect = reader.u32();
        }
        const uint32_t format = reader.u32();
        uint32_t canonical_size = 0;
        const uint8_t *bytes =
            reader.bytes(exact_command_max_bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX,
                                                 AYNI_GOAL_PARAMETER_MAX_BYTES_MAX, 0,
                                                 knowledge::memory_body_max),
                         canonical_size);
        canonical = {bytes, canonical_size};
        mapping.canonical_size = canonical_size;
        if (version == work_authority_command_record_version) {
            uint32_t result_size = 0;
            const auto *result_bytes = reader.bytes(
                work_authority_result_size(AYNI_GOAL_REQUIREMENT_CAPACITY_MAX), result_size);
            mapping.result_size = result_size;
            result = {result_bytes, result_size};
        }
        const auto expected_kind = version == revoke_command_record_version
                                       ? AYNI_INTENT_GRANT_REVOKE
                                       : AYNI_INTENT_GRANT_CREATE;
        const bool right_kind = version == memory_command_record_version
                                    ? is_memory_command(mapping.kind)
                                : version == scene_command_record_version
                                    ? mapping.kind == AYNI_INTENT_SCENE_SET_ENABLED
                                : version == resume_wait_command_record_version
                                    ? mapping.kind == AYNI_INTENT_TASK_RESUME_WAIT
                                : version == call_query_command_record_version
                                    ? mapping.kind == AYNI_INTENT_CALL_QUERY_RESULT
                                : version == work_authority_command_record_version
                                    ? is_work_authority_command(mapping.kind) && result.size != 0U
                                : version == task_limit_command_record_version
                                    ? mapping.kind == AYNI_INTENT_TASK_RAISE_MODEL_LIMIT
                                    : mapping.kind == expected_kind;
        if (format != authority_command_format || !right_kind ||
            !valid_exact_mapping(mapping, canonical, result)) {
            return false;
        }
    } else if (!is_ordinary_command(mapping.kind)) {
        return false;
    }
    if (!reader.ok()) {
        return false;
    }
    out_mapping = mapping;
    out_floor = floor;
    out_canonical = canonical;
    if (out_result != nullptr) {
        *out_result = result;
    }
    return true;
}

bool read_command_record(const uint8_t *body, uint32_t size, CommandMapping &out_mapping,
                         uint64_t &out_floor) {
    ayni_bytes_v1 ignored{};
    return read_command_record(command_record_version, body, size, out_mapping, out_floor, ignored);
}

} // namespace ayni::kernel
