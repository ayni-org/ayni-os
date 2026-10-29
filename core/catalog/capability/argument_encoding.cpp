#include "catalog/capability/argument_encoding.h"
#include "catalog/capability/argument_mapping.h"
#include "foundation/record_body.h"

namespace ayni::catalog {
namespace {
constexpr uint32_t word_bytes = sizeof(uint32_t);
constexpr uint32_t header_bytes = word_bytes * 2U;     // Version/count, records.md named arguments.
constexpr uint32_t node_fixed_bytes = word_bytes * 3U; // Kind/end/name length, no ABI padding.
uint32_t argument_size(const ayni_arguments_v1 &input) {
    uint32_t size = header_bytes;
    for (uint32_t at = 0; at < input.node_count; ++at) {
        const auto &node = input.nodes[at];
        size += node_fixed_bytes + node.name.size;
        if (node.kind == AYNI_ARGUMENT_BOOL) {
            size += word_bytes;
        } else if (node.kind == AYNI_ARGUMENT_NUMBER || node.kind == AYNI_ARGUMENT_STRING) {
            size += foundation::string_field_size(node.value.text.size);
        }
    }
    return size;
}
} // namespace

// Shape node/text limits bound every addition before this codec starts.
// Inactive union members and reserved/padding bytes are never serialized.
ayni_status_t encode_arguments(const ayni_arguments_v1 &input, uint8_t *out, uint32_t capacity,
                               uint32_t &size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto checked = validate_arguments(input);
    if (checked != AYNI_OK) {
        return checked;
    }
    const uint32_t required = argument_size(input);
    if (required > capacity) {
        size = required;
        return AYNI_BUFFER_TOO_SMALL;
    }
    foundation::BodyWriter writer(out, required);
    writer.u32(argument_encoding_version);
    writer.u32(input.node_count);
    for (uint32_t at = 0; at < input.node_count; ++at) {
        const auto &node = input.nodes[at];
        writer.u32(node.kind);
        writer.u32(node.subtree_end);
        writer.text(node.name);
        if (node.kind == AYNI_ARGUMENT_BOOL) {
            writer.u32(node.value.boolean);
        } else if (node.kind == AYNI_ARGUMENT_NUMBER || node.kind == AYNI_ARGUMENT_STRING) {
            writer.text(node.value.text);
        }
    }
    size = required;
    return AYNI_OK;
}

// Decode unpublished nodes, then reuse the struct validator. Number spellings
// remain text until the original business Schema decides their precise type.
ayni_status_t decode_arguments(ayni_bytes_v1 body, DecodedArguments &out) {
    if (body.data == nullptr || body.size < header_bytes) {
        return AYNI_INVALID_ARGUMENT;
    }
    foundation::BodyReader reader(body);
    if (reader.u32() != argument_encoding_version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    DecodedArguments candidate;
    candidate.count = reader.u32();
    if (candidate.count == 0U || candidate.count > candidate.nodes.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < candidate.count; ++at) {
        auto &node = candidate.nodes[at];
        node.kind = reader.u32();
        node.subtree_end = reader.u32();
        node.name = reader.text(AYNI_SCHEMA_NAME_BYTES_MAX);
        if (node.kind == AYNI_ARGUMENT_BOOL) {
            node.value.boolean = reader.u32();
        } else if (node.kind == AYNI_ARGUMENT_NUMBER || node.kind == AYNI_ARGUMENT_STRING) {
            node.value.text = reader.text(AYNI_VALUE_BYTES_MAX);
        }
    }
    if (!reader.ok()) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto checked = validate_arguments(candidate.view());
    if (checked == AYNI_OK) {
        out = candidate;
    }
    return checked;
}
} // namespace ayni::catalog
