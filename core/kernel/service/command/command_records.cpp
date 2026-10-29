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

// Content state is orthogonal to command kind. A tombstone never carries private bytes.
enum class CommandContent : uint32_t { retained = 1, erased = 2 };
constexpr uint32_t canonical_bytes_max =
    exact_command_max_bytes(AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX,
                            AYNI_GOAL_PARAMETER_MAX_BYTES_MAX, 0, knowledge::memory_body_max);
constexpr uint32_t result_bytes_max =
    work_authority_result_size(AYNI_GOAL_REQUIREMENT_CAPACITY_MAX);

bool valid_payload(const CommandMapping &mapping, ayni_bytes_v1 canonical, ayni_bytes_v1 result) {
    if (mapping.canonical_size != canonical.size || mapping.result_size != result.size ||
        !valid_command_sources(mapping)) {
        return false;
    }
    if (mapping.privacy_expired) {
        return valid_expired_mapping(mapping) && canonical.size == 0 && result.size == 0;
    }
    return canonical.size == 0 ? result.size == 0 && is_ordinary_command(mapping.kind)
                                : valid_exact_mapping(mapping, canonical, result);
}
} // namespace

bool valid_command_sources(const CommandMapping &mapping) {
    return knowledge::valid_dependencies(mapping.sources, knowledge::memory_dependency_max) &&
           (mapping.sources.count == 0 ||
            (!mapping.privacy_expired && mapping.object.kind == AYNI_OBJECT_GOAL &&
             (is_ordinary_command(mapping.kind) || is_work_authority_command(mapping.kind))));
}

uint32_t command_record_size_for(const CommandMapping &mapping) {
    if (!valid_command_sources(mapping) || mapping.canonical_size > canonical_bytes_max ||
        mapping.result_size > result_bytes_max ||
        (mapping.privacy_expired && !valid_expired_mapping(mapping))) {
        return 0;
    }
    return command_record_fixed_size + mapping.canonical_size + mapping.result_size +
           knowledge::memory_dependencies_size(mapping.sources.count);
}

// One current envelope serves ordinary, exact and erased facts without nesting older layouts.
bool write_command_record(const CommandMapping &mapping, uint64_t floor, ayni_bytes_v1 canonical,
                          uint8_t *body, uint32_t size, ayni_bytes_v1 result) {
    if (body == nullptr || size == 0 || size != command_record_size_for(mapping) ||
        !valid_payload(mapping, canonical, result)) {
        return false;
    }
    durability::BodyWriter writer(body, size);
    write_fixed(mapping, floor, writer);
    writer.u32(static_cast<uint32_t>(mapping.privacy_expired ? CommandContent::erased
                                                           : CommandContent::retained));
    writer.u32(mapping.effect);
    writer.u32(canonical.size != 0 ? authority_command_format : 0);
    writer.bytes(canonical.data, canonical.size);
    writer.bytes(result.data, result.size);
    knowledge::write_memory_dependencies(mapping.sources, writer);
    return writer.complete();
}

void write_command_record(const CommandMapping &mapping, uint64_t floor, uint8_t *body) {
    static_cast<void>(write_command_record(mapping, floor, {}, body, command_record_size));
}

// Parse into borrowed temporary views. The same payload validator owns all intrinsic rules.
bool read_command_record(const uint8_t *body, uint32_t size,
                         CommandMapping &out_mapping, uint64_t &out_floor,
                         ayni_bytes_v1 &out_canonical, ayni_bytes_v1 *out_result) {
    if (body == nullptr || size < command_record_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    uint64_t floor = 0;
    CommandMapping mapping = read_fixed(reader, floor);
    const auto content = reader.u32();
    if (content != static_cast<uint32_t>(CommandContent::retained) &&
        content != static_cast<uint32_t>(CommandContent::erased)) {
        return false;
    }
    mapping.privacy_expired = content == static_cast<uint32_t>(CommandContent::erased);
    mapping.effect = reader.u32();
    const auto format = reader.u32();
    uint32_t canonical_size = 0;
    const auto *canonical_data = reader.bytes(canonical_bytes_max, canonical_size);
    ayni_bytes_v1 canonical{canonical_data, canonical_size};
    mapping.canonical_size = canonical_size;
    uint32_t result_size = 0;
    const auto *result_data = reader.bytes(result_bytes_max, result_size);
    ayni_bytes_v1 result{result_data, result_size};
    mapping.result_size = result_size;
    if (!knowledge::read_memory_dependencies(reader, mapping.sources) || !reader.ok() ||
        format != (canonical_size != 0 ? authority_command_format : 0) ||
        !valid_payload(mapping, canonical, result)) {
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
    return read_command_record(body, size, out_mapping, out_floor, ignored);
}

} // namespace ayni::kernel
