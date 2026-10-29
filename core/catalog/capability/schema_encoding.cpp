#include "catalog/capability/schema_encoding.h"
#include "catalog/capability/field_values.h"
#include "catalog/capability/value_validation.h"
#include "catalog/catalog_id.h"
#include "foundation/record_body.h"

#include <cstring>

namespace ayni::catalog {
namespace {
constexpr uint32_t word_bytes = sizeof(uint32_t);
constexpr uint32_t wide_bytes = sizeof(uint64_t);
constexpr uint32_t schema_header_bytes = word_bytes + word_bytes;

using Output = foundation::BodyWriter;
using Input = foundation::BodyReader;

using Order = std::array<uint32_t, AYNI_SCHEMA_NODE_COUNT_MAX>;

// Stable field ID order is independent of caller registration layout; list
// items retain their one declared shape. Schema depth was admitted beforehand.
Order canonical_order(const ayni_schema_v1 &schema) {
    Order order{};
    Order pending{};
    uint32_t count = 0;
    uint32_t occupied = 1;
    pending[0] = 0;
    while (occupied != 0U) {
        const uint32_t node = pending[--occupied];
        order[count++] = node;
        Order children{};
        uint32_t size = 0;
        for (uint32_t at = node + 1U; at < schema.nodes[node].subtree_end;
             at = schema.nodes[at].subtree_end) {
            uint32_t position = size;
            while (position != 0U &&
                   schema.nodes[children[position - 1U]].field_id > schema.nodes[at].field_id) {
                children[position] = children[position - 1U];
                --position;
            }
            children[position] = at;
            ++size;
        }
        while (size != 0U) {
            pending[occupied++] = children[--size];
        }
    }
    return order;
}

void write_choices(const ayni_schema_node_v1 &node, Output &out) {
    std::array<uint32_t, AYNI_VALUE_LIST_COUNT_MAX> order{};
    for (uint32_t at = 0; at < node.choice_count; ++at) {
        uint32_t position = at;
        while (position != 0U &&
               compare_tokens(node.choices[order[position - 1U]], node.choices[at]) > 0) {
            order[position] = order[position - 1U];
            --position;
        }
        order[position] = at;
    }
    out.u32(node.choice_count);
    for (uint32_t at = 0; at < node.choice_count; ++at) {
        out.text(node.choices[order[at]]);
    }
}

// Only the kind's bounds participate in exact identity. Zeroed irrelevant
// C fields and ABI padding cannot create an accidental compatibility rule.
void write_bounds(const ayni_schema_node_v1 &node, Output &out) {
    switch (node.kind) {
    case AYNI_VALUE_I64:
    case AYNI_VALUE_DECIMAL:
        out.u64(static_cast<uint64_t>(node.min_i64));
        out.u64(static_cast<uint64_t>(node.max_i64));
        if (node.kind == AYNI_VALUE_DECIMAL) {
            out.u32(node.scale);
        }
        break;
    case AYNI_VALUE_U64:
        out.u64(node.min_u64);
        out.u64(node.max_u64);
        break;
    case AYNI_VALUE_FLOAT64: {
        const double minimum = node.min_float64 == 0.0 ? 0.0 : node.min_float64;
        const double maximum = node.max_float64 == 0.0 ? 0.0 : node.max_float64;
        uint64_t low = 0, high = 0;
        std::memcpy(&low, &minimum, wide_bytes);
        std::memcpy(&high, &maximum, wide_bytes);
        out.u64(low);
        out.u64(high);
        break;
    }
    case AYNI_VALUE_TEXT:
    case AYNI_VALUE_LIST:
        out.u32(node.min_count);
        out.u32(node.max_count);
        break;
    case AYNI_VALUE_ENUM:
    case AYNI_VALUE_IDENTIFIER:
        write_choices(node, out);
        break;
    default:
        break;
    }
}

ayni_status_t write_default(const ayni_schema_v1 &schema, uint32_t field, Output &out,
                            uint32_t &default_nodes) {
    if (schema.nodes[field].default_value == nullptr) {
        out.u32(0);
        return AYNI_OK;
    }
    std::array<ayni_value_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
    std::array<char, schema_encoding_bytes_max> bytes{};
    ayni_value_buffer_v1 buffer{nodes.data(), static_cast<uint32_t>(nodes.size()), 0,
                                bytes.data(), static_cast<uint32_t>(bytes.size()), 0};
    const auto normalized = normalize_schema_default(schema, field, buffer);
    if (normalized != AYNI_OK) {
        return normalized == AYNI_BUFFER_TOO_SMALL ? AYNI_CAPACITY_EXCEEDED : normalized;
    }
    if (buffer.node_count > schema_default_node_count_max - default_nodes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    default_nodes += buffer.node_count;
    std::array<uint8_t, schema_encoding_bytes_max> encoded{};
    uint32_t size = 0;
    const ayni_value_v1 value{buffer.nodes, buffer.node_count};
    const auto status =
        encode_value(value, encoded.data(), static_cast<uint32_t>(encoded.size()), size);
    if (status != AYNI_OK) {
        return status == AYNI_BUFFER_TOO_SMALL ? AYNI_CAPACITY_EXCEEDED : status;
    }
    out.bytes(encoded.data(), size);
    return out.valid() ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}

bool read_choices(Input &input, ayni_schema_node_v1 &node, DecodedSchema &schema) {
    node.choice_count = input.u32();
    if (node.choice_count > schema.choices.size() - schema.choice_count ||
        node.choice_count > AYNI_VALUE_LIST_COUNT_MAX) {
        return false;
    }
    node.choices = node.choice_count == 0U ? nullptr : schema.choices.data() + schema.choice_count;
    for (uint32_t at = 0; at < node.choice_count; ++at) {
        auto &choice = schema.choices[schema.choice_count++];
        choice = input.text(schema_encoding_bytes_max);
        if (!input.valid() || (at != 0U && compare_tokens(node.choices[at - 1U], choice) >= 0)) {
            return false;
        }
    }
    return true;
}

bool read_bounds(Input &input, ayni_schema_node_v1 &node, DecodedSchema &schema) {
    switch (node.kind) {
    case AYNI_VALUE_I64:
    case AYNI_VALUE_DECIMAL: {
        const uint64_t low = input.u64(), high = input.u64();
        std::memcpy(&node.min_i64, &low, wide_bytes);
        std::memcpy(&node.max_i64, &high, wide_bytes);
        if (node.kind == AYNI_VALUE_DECIMAL) {
            node.scale = input.u32();
        }
        break;
    }
    case AYNI_VALUE_U64:
        node.min_u64 = input.u64();
        node.max_u64 = input.u64();
        break;
    case AYNI_VALUE_FLOAT64: {
        const uint64_t low = input.u64(), high = input.u64();
        std::memcpy(&node.min_float64, &low, wide_bytes);
        std::memcpy(&node.max_float64, &high, wide_bytes);
        break;
    }
    case AYNI_VALUE_TEXT:
    case AYNI_VALUE_LIST:
        node.min_count = input.u32();
        node.max_count = input.u32();
        break;
    case AYNI_VALUE_ENUM:
    case AYNI_VALUE_IDENTIFIER:
        return read_choices(input, node, schema);
    default:
        break;
    }
    return input.valid();
}

bool read_default(Input &input, uint32_t field, DecodedSchema &schema) {
    const auto body = input.bytes(schema_encoding_bytes_max);
    if (!input.valid()) {
        return false;
    }
    if (body.size == 0U) {
        return true;
    }
    DecodedValue value;
    if (decode_value(body, value) != AYNI_OK || !canonical_value(value.view()) ||
        value.count > schema.default_nodes.size() - schema.default_count) {
        return false;
    }
    auto *nodes = schema.default_nodes.data() + schema.default_count;
    for (uint32_t at = 0; at < value.count; ++at) {
        nodes[at] = value.nodes[at];
    }
    schema.defaults[field] = {nodes, value.count};
    schema.nodes[field].default_value = &schema.defaults[field];
    schema.default_count += value.count;
    return true;
}
} // namespace

DecodedSchema::DecodedSchema(const DecodedSchema &source) {
    *this = source;
}
DecodedSchema &DecodedSchema::operator=(const DecodedSchema &source) {
    if (this == &source) {
        return *this;
    }
    nodes = source.nodes;
    choices = source.choices;
    defaults = source.defaults;
    default_nodes = source.default_nodes;
    count = source.count;
    choice_count = source.choice_count;
    default_count = source.default_count;
    for (uint32_t at = 0; at < count; ++at) {
        if (nodes[at].choices != nullptr) {
            nodes[at].choices = choices.data() + (source.nodes[at].choices - source.choices.data());
        }
        if (nodes[at].default_value != nullptr) {
            defaults[at].nodes =
                default_nodes.data() + (source.defaults[at].nodes - source.default_nodes.data());
            nodes[at].default_value = &defaults[at];
        }
    }
    return *this;
}

ayni_status_t encode_schema(const ayni_schema_v1 &schema, uint8_t *out, uint32_t capacity,
                            uint32_t &size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto checked = validate_schema(schema);
    if (checked != AYNI_OK) {
        return checked;
    }
    return encode_validated_schema(schema, out, capacity, size);
}

ayni_status_t encode_validated_schema(const ayni_schema_v1 &schema, uint8_t *out, uint32_t capacity,
                                      uint32_t &size) {
    std::array<uint8_t, schema_encoding_bytes_max> bytes{};
    Output writer(bytes.data(), static_cast<uint32_t>(bytes.size()));
    writer.u32(schema_encoding_version);
    writer.u32(schema.node_count);
    const auto order = canonical_order(schema);
    uint32_t choices = 0, defaults = 0;
    for (uint32_t at = 0; at < schema.node_count; ++at) {
        const uint32_t old = order[at];
        const auto &node = schema.nodes[old];
        writer.u32(node.kind);
        writer.u32(node.field_id);
        writer.u32(at + node.subtree_end - old);
        writer.u32(node.flags);
        writer.text(node.name);
        writer.text(node.unit);
        write_bounds(node, writer);
        const auto status = write_default(schema, old, writer, defaults);
        if (status != AYNI_OK) {
            return status;
        }
        if (node.kind == AYNI_VALUE_ENUM || node.kind == AYNI_VALUE_IDENTIFIER) {
            choices += node.choice_count;
        }
    }
    if (!writer.valid() || choices > schema_choice_count_max ||
        defaults > schema_default_node_count_max) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (writer.position() > capacity) {
        size = writer.position();
        return AYNI_BUFFER_TOO_SMALL;
    }
    std::memcpy(out, bytes.data(), writer.position());
    size = writer.position();
    return AYNI_OK;
}

ayni_status_t decode_schema(ayni_bytes_v1 body, DecodedSchema &out) {
    if (body.data == nullptr || body.size < schema_header_bytes ||
        body.size > schema_encoding_bytes_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    Input input(body);
    if (input.u32() != schema_encoding_version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    DecodedSchema schema;
    schema.count = input.u32();
    if (schema.count == 0U || schema.count > schema.nodes.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < schema.count; ++at) {
        auto &node = schema.nodes[at];
        node.kind = input.u32();
        node.field_id = input.u32();
        node.subtree_end = input.u32();
        node.flags = input.u32();
        node.name = input.text(AYNI_SCHEMA_NAME_BYTES_MAX);
        node.unit = input.text(AYNI_CATALOG_ID_MAX_BYTES);
        if (!read_bounds(input, node, schema) || !read_default(input, at, schema)) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    if (!input.ok()) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto checked = validate_schema(schema.view());
    if (checked != AYNI_OK) {
        return checked;
    }
    std::array<uint8_t, schema_encoding_bytes_max> canonical{};
    uint32_t size = 0;
    const auto encoded = encode_validated_schema(schema.view(), canonical.data(),
                                                 static_cast<uint32_t>(canonical.size()), size);
    if (encoded != AYNI_OK || size != body.size ||
        std::memcmp(body.data, canonical.data(), size) != 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = schema;
    return AYNI_OK;
}
} // namespace ayni::catalog
