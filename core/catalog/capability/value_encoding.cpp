#include "catalog/capability/value_encoding.h"
#include "catalog/capability/value_validation.h"
#include "catalog/capability/workspace.h"
#include "foundation/little_endian.h"

#include <cstring>
#include <limits>

namespace ayni::catalog {
namespace {
constexpr uint32_t word_bytes = sizeof(uint32_t);
constexpr uint32_t wide_bytes = sizeof(uint64_t);
constexpr uint32_t kind_at = 0;
constexpr uint32_t field_at = word_bytes;
constexpr uint32_t end_at = field_at + word_bytes;
constexpr uint64_t negative_zero_bits = uint64_t{1} << 63U;

bool string_kind(ayni_value_kind_t kind) {
    return kind == AYNI_VALUE_TEXT || kind == AYNI_VALUE_ENUM || kind == AYNI_VALUE_IDENTIFIER;
}
uint32_t scalar_bytes(const ayni_value_node_v1 &node) {
    switch (node.kind) {
    case AYNI_VALUE_BOOL:
        return word_bytes;
    case AYNI_VALUE_I64:
    case AYNI_VALUE_U64:
    case AYNI_VALUE_FLOAT64:
        return wide_bytes;
    case AYNI_VALUE_DECIMAL:
        return wide_bytes + word_bytes;
    default:
        return 0;
    }
}

ayni_status_t measure(const ayni_value_v1 &value, uint32_t &out) {
    uint32_t size = value_encoding_header_bytes;
    for (uint32_t at = 0; at < value.node_count; ++at) {
        const auto &node = value.nodes[at];
        const uint64_t extra =
            static_cast<uint64_t>(value_node_header_bytes) + scalar_bytes(node) +
            (string_kind(node.kind) ? static_cast<uint64_t>(word_bytes) + node.value.text.size
                                    : 0U);
        if (extra > std::numeric_limits<uint32_t>::max() - size) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        size += static_cast<uint32_t>(extra);
    }
    out = size;
    return AYNI_OK;
}

// Node bytes encode only the active payload. In particular a decimal is not
// raw union memory, and double is represented by its IEEE bit pattern.
uint32_t write_node(const ayni_value_node_v1 &node, uint8_t *out) {
    foundation::store_le32(out + kind_at, node.kind);
    foundation::store_le32(out + field_at, node.field_id);
    foundation::store_le32(out + end_at, node.subtree_end);
    auto *payload = out + value_node_header_bytes;
    const uint32_t scalar = scalar_bytes(node);
    switch (node.kind) {
    case AYNI_VALUE_BOOL:
        foundation::store_le32(payload, node.value.boolean);
        break;
    case AYNI_VALUE_I64:
        foundation::store_le64(payload, static_cast<uint64_t>(node.value.integer));
        break;
    case AYNI_VALUE_U64:
        foundation::store_le64(payload, node.value.unsigned_integer);
        break;
    case AYNI_VALUE_FLOAT64: {
        static_assert(sizeof(double) == wide_bytes && std::numeric_limits<double>::is_iec559,
                      "business floating values require IEEE binary64");
        uint64_t bits = 0;
        std::memcpy(&bits, &node.value.approximate, sizeof(bits));
        foundation::store_le64(payload, bits);
        break;
    }
    case AYNI_VALUE_DECIMAL:
        foundation::store_le64(payload, static_cast<uint64_t>(node.value.decimal.coefficient));
        foundation::store_le32(payload + wide_bytes, node.value.decimal.scale);
        break;
    default:
        break;
    }
    if (string_kind(node.kind)) {
        foundation::store_le32(payload, node.value.text.size);
        if (node.value.text.size != 0U) {
            std::memcpy(payload + word_bytes, node.value.text.data, node.value.text.size);
        }
        return value_node_header_bytes + word_bytes + node.value.text.size;
    }
    return value_node_header_bytes + scalar;
}

// Signed bit patterns round-trip without implementation-defined unsigned-to-
// signed conversion; scalar shape admission follows the complete decode.
void read_scalar(ayni_value_node_v1 &node, const uint8_t *payload) {
    uint64_t bits = 0;
    switch (node.kind) {
    case AYNI_VALUE_BOOL:
        node.value.boolean = foundation::load_le32(payload);
        break;
    case AYNI_VALUE_U64:
        node.value.unsigned_integer = foundation::load_le64(payload);
        break;
    case AYNI_VALUE_I64:
        bits = foundation::load_le64(payload);
        std::memcpy(&node.value.integer, &bits, wide_bytes);
        break;
    case AYNI_VALUE_FLOAT64:
        bits = foundation::load_le64(payload);
        std::memcpy(&node.value.approximate, &bits, wide_bytes);
        break;
    case AYNI_VALUE_DECIMAL:
        bits = foundation::load_le64(payload);
        std::memcpy(&node.value.decimal.coefficient, &bits, wide_bytes);
        node.value.decimal.scale = foundation::load_le32(payload + wide_bytes);
        break;
    default:
        break;
    }
}

bool read_node(ayni_bytes_v1 body, uint32_t &offset, ayni_value_node_v1 &node) {
    if (body.size - offset < value_node_header_bytes) {
        return false;
    }
    const auto *header = body.data + offset;
    node.kind = foundation::load_le32(header + kind_at);
    node.field_id = foundation::load_le32(header + field_at);
    node.subtree_end = foundation::load_le32(header + end_at);
    offset += value_node_header_bytes;
    uint32_t size = scalar_bytes(node);
    if (string_kind(node.kind)) {
        if (body.size - offset < word_bytes) {
            return false;
        }
        size = foundation::load_le32(body.data + offset);
        offset += word_bytes;
        if (size > body.size - offset) {
            return false;
        }
        node.value.text = {reinterpret_cast<const char *>(body.data + offset), size};
    } else {
        if (size > body.size - offset) {
            return false;
        }
        read_scalar(node, body.data + offset);
    }
    offset += size;
    return true;
}
} // namespace

ayni_status_t measure_admitted_value(const ayni_value_v1 &value, uint32_t &size) {
    return measure(value, size);
}
ayni_status_t encode_value(const ayni_value_v1 &value, uint8_t *out, uint32_t capacity,
                           uint32_t &size) {
    const auto status = validate_value(value);
    return status == AYNI_OK ? encode_admitted_value(value, out, capacity, size) : status;
}
ayni_status_t encode_admitted_value(const ayni_value_v1 &value, uint8_t *out, uint32_t capacity,
                                    uint32_t &size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t required = 0;
    const auto status = measure(value, required);
    if (status != AYNI_OK) {
        return status;
    }
    if (required > capacity) {
        size = required;
        return AYNI_BUFFER_TOO_SMALL;
    }
    foundation::store_le32(out, value_encoding_version);
    foundation::store_le32(out + word_bytes, value.node_count);
    uint32_t offset = value_encoding_header_bytes;
    for (uint32_t at = 0; at < value.node_count; ++at) {
        offset += write_node(value.nodes[at], out + offset);
    }
    size = required;
    return AYNI_OK;
}

ayni_status_t decode_value(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body,
                           DecodedValue &out) {
    if (body.data == nullptr || body.size < value_encoding_header_bytes ||
        foundation::load_le32(body.data) != value_encoding_version) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto decoded_lease = workspace.make<DecodedValue>();
    if (!decoded_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &decoded = *decoded_lease;
    decoded.count = foundation::load_le32(body.data + word_bytes);
    if (decoded.count == 0U || decoded.count > decoded.nodes.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t offset = value_encoding_header_bytes;
    for (uint32_t at = 0; at < decoded.count; ++at) {
        if (!read_node(body, offset, decoded.nodes[at])) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    if (offset != body.size) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto status = validate_value(decoded.view());
    if (status == AYNI_OK) {
        out = decoded;
    }
    return status;
}

// Admitted trees only: inactive union members and native padding have no meaning.
bool same_value(const ayni_value_v1 &left, const ayni_value_v1 &right) {
    if (left.node_count != right.node_count) {
        return false;
    }
    for (uint32_t at = 0; at < left.node_count; ++at) {
        const auto &a = left.nodes[at];
        const auto &b = right.nodes[at];
        if (a.kind != b.kind || a.field_id != b.field_id || a.subtree_end != b.subtree_end) {
            return false;
        }
        switch (a.kind) {
        case AYNI_VALUE_BOOL:
            if (a.value.boolean != b.value.boolean) {
                return false;
            }
            break;
        case AYNI_VALUE_I64:
            if (a.value.integer != b.value.integer) {
                return false;
            }
            break;
        case AYNI_VALUE_U64:
            if (a.value.unsigned_integer != b.value.unsigned_integer) {
                return false;
            }
            break;
        case AYNI_VALUE_FLOAT64:
            if (std::memcmp(&a.value.approximate, &b.value.approximate, sizeof(double)) != 0) {
                return false;
            }
            break;
        case AYNI_VALUE_DECIMAL:
            if (a.value.decimal.coefficient != b.value.decimal.coefficient ||
                a.value.decimal.scale != b.value.decimal.scale) {
                return false;
            }
            break;
        case AYNI_VALUE_TEXT:
        case AYNI_VALUE_ENUM:
        case AYNI_VALUE_IDENTIFIER:
            if (a.value.text.size != b.value.text.size ||
                (a.value.text.size != 0U &&
                 std::memcmp(a.value.text.data, b.value.text.data, a.value.text.size) != 0)) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    return true;
}

bool canonical_value(const ayni_value_v1 &value) {
    if (validate_value(value) != AYNI_OK) {
        return false;
    }
    for (uint32_t at = 0; at < value.node_count; ++at) {
        const auto &node = value.nodes[at];
        if (node.kind == AYNI_VALUE_OBJECT) {
            uint32_t previous = 0;
            for (uint32_t child = at + 1U; child < node.subtree_end;
                 child = value.nodes[child].subtree_end) {
                if (value.nodes[child].field_id <= previous) {
                    return false;
                }
                previous = value.nodes[child].field_id;
            }
        } else if (node.kind == AYNI_VALUE_FLOAT64) {
            uint64_t bits = 0;
            std::memcpy(&bits, &node.value.approximate, sizeof(bits));
            if (bits == negative_zero_bits) {
                return false;
            }
        }
    }
    return true;
}
} // namespace ayni::catalog

extern "C" ayni_status_t ayni_value_write(const ayni_value_v1 *value, uint8_t *out,
                                          uint32_t capacity, uint32_t *out_size) {
    if (value == nullptr || out_size == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    return ayni::catalog::encode_value(*value, out, capacity, *out_size);
}

extern "C" ayni_status_t ayni_value_read(const ayni_workspace_v1 *region, ayni_bytes_v1 body,
                                         ayni_value_node_v1 *nodes, uint32_t capacity,
                                         uint32_t *out_count) {
    const auto workspace_status =
        ayni::foundation::Workspace::check(region, ayni::catalog::value_workspace_size());
    if (workspace_status != AYNI_OK) {
        return workspace_status;
    }
    ayni::foundation::Workspace workspace(region->data, region->byte_capacity);

    if (out_count == nullptr || (capacity != 0U && nodes == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto decoded_lease = workspace.make<ayni::catalog::DecodedValue>();
    if (!decoded_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &decoded = *decoded_lease;
    const auto status = ayni::catalog::decode_value(workspace, body, decoded);
    if (status != AYNI_OK) {
        return status;
    }
    if (capacity < decoded.count) {
        *out_count = decoded.count;
        return AYNI_BUFFER_TOO_SMALL;
    }
    for (uint32_t at = 0; at < decoded.count; ++at) {
        nodes[at] = decoded.nodes[at];
    }
    *out_count = decoded.count;
    return AYNI_OK;
}
