#include "catalog/capability/value_validation.h"
#include "catalog/catalog_id.h"
#include "foundation/utf8.h"

#include <array>
#include <cstring>
#include <limits>

namespace ayni::catalog {
namespace {
constexpr uint32_t schema_flags = AYNI_SCHEMA_REQUIRED | AYNI_SCHEMA_NULLABLE;
constexpr uint32_t decimal_radix = 10;

bool finite_real(double value) {
    constexpr double bound = std::numeric_limits<double>::max();
    return value >= -bound && value <= bound;
}

bool text_valid(ayni_string_v1 text) {
    return (text.size == 0U || text.data != nullptr) &&
           foundation::valid_utf8(text.data, text.size);
}
bool same_text(ayni_string_v1 left, ayni_string_v1 right) {
    return left.size == right.size &&
           (left.size == 0U || std::memcmp(left.data, right.data, left.size) == 0);
}

// Both public trees use this one structural walk. Explicit parents prove
// reachability, noncrossing intervals and the depth bound before semantic use.
template <typename Node> bool tree_shape(const Node *nodes, uint32_t count, uint32_t limit) {
    if (nodes == nullptr || count == 0U || count > limit || nodes[0].field_id != 0U ||
        nodes[0].subtree_end != count) {
        return false;
    }
    std::array<uint32_t, AYNI_VALUE_DEPTH_MAX> parents{};
    uint32_t depth = 0;
    for (uint32_t index = 0; index < count; ++index) {
        while (depth != 0U && nodes[parents[depth - 1U]].subtree_end == index) {
            --depth;
        }
        const auto &node = nodes[index];
        if (node.kind > AYNI_VALUE_TEXT || node.subtree_end <= index || node.subtree_end > count ||
            depth >= AYNI_VALUE_DEPTH_MAX ||
            (!value_container(node.kind) && node.subtree_end != index + 1U)) {
            return false;
        }
        if (index != 0U) {
            if (depth == 0U) {
                return false;
            }
            const auto &parent = nodes[parents[depth - 1U]];
            if (node.subtree_end > parent.subtree_end ||
                (parent.kind == AYNI_VALUE_OBJECT) != (node.field_id != 0U)) {
                return false;
            }
            if (parent.kind == AYNI_VALUE_OBJECT) {
                for (uint32_t before = parents[depth - 1U] + 1U; before < index;
                     before = nodes[before].subtree_end) {
                    if (nodes[before].field_id == node.field_id) {
                        return false;
                    }
                }
            }
        }
        if (value_container(node.kind)) {
            parents[depth++] = index;
        }
    }
    return true;
}

uint32_t children(const ayni_schema_node_v1 *nodes, uint32_t index) {
    uint32_t count = 0;
    for (uint32_t child = index + 1U; child < nodes[index].subtree_end;
         child = nodes[child].subtree_end) {
        ++count;
    }
    return count;
}

bool choices_valid(const ayni_schema_node_v1 &node) {
    if ((node.choice_count != 0U) != (node.choices != nullptr) ||
        node.choice_count > AYNI_VALUE_LIST_COUNT_MAX ||
        (node.kind == AYNI_VALUE_ENUM && node.choice_count == 0U)) {
        return false;
    }
    for (uint32_t at = 0; at < node.choice_count; ++at) {
        const auto value = node.choices[at];
        if (value.size > AYNI_VALUE_BYTES_MAX || !text_valid(value) || value.size == 0U ||
            (node.kind == AYNI_VALUE_IDENTIFIER && !CatalogId::is_valid(value))) {
            return false;
        }
        for (uint32_t before = 0; before < at; ++before) {
            if (same_text(value, node.choices[before])) {
                return false;
            }
        }
    }
    return true;
}

// A declaration's finite bounds are checked once here, independent of values.
bool declaration_valid(const ayni_schema_v1 &schema, uint32_t index) {
    const auto &node = schema.nodes[index];
    if (node.kind == AYNI_VALUE_NULL || (node.flags & ~schema_flags) != 0U ||
        node.name.size > AYNI_SCHEMA_NAME_BYTES_MAX || node.unit.size > AYNI_CATALOG_ID_MAX_BYTES ||
        !text_valid(node.name) || !text_valid(node.unit) ||
        ((node.field_id == 0U) != (node.name.size == 0U)) ||
        (node.default_value != nullptr &&
         (node.field_id == 0U || (node.flags & AYNI_SCHEMA_REQUIRED) != 0U))) {
        return false;
    }
    switch (node.kind) {
    case AYNI_VALUE_OBJECT:
        for (uint32_t child = index + 1U; child < node.subtree_end;
             child = schema.nodes[child].subtree_end) {
            for (uint32_t before = index + 1U; before < child;
                 before = schema.nodes[before].subtree_end) {
                if (same_text(schema.nodes[before].name, schema.nodes[child].name)) {
                    return false;
                }
            }
        }
        return true;
    case AYNI_VALUE_LIST:
        return children(schema.nodes, index) == 1U && node.min_count <= node.max_count &&
               node.max_count <= AYNI_VALUE_LIST_COUNT_MAX;
    case AYNI_VALUE_I64:
        return node.min_i64 <= node.max_i64;
    case AYNI_VALUE_U64:
        return node.min_u64 <= node.max_u64;
    case AYNI_VALUE_FLOAT64:
        return finite_real(node.min_float64) && finite_real(node.max_float64) &&
               node.min_float64 <= node.max_float64;
    case AYNI_VALUE_DECIMAL:
        return node.min_i64 <= node.max_i64 && node.scale <= AYNI_DECIMAL_SCALE_MAX;
    case AYNI_VALUE_TEXT:
        return node.min_count <= node.max_count;
    case AYNI_VALUE_ENUM:
    case AYNI_VALUE_IDENTIFIER:
        return choices_valid(node);
    case AYNI_VALUE_BOOL:
        return true;
    default:
        return false;
    }
}
} // namespace

bool value_container(ayni_value_kind_t kind) {
    return kind == AYNI_VALUE_OBJECT || kind == AYNI_VALUE_LIST;
}

// Shape validation never reads inactive union storage (ADR-0084). Container
// counts are independent of child subtree lengths; null has no active payload.
ayni_status_t validate_value(const ayni_value_v1 &value) {
    if (!tree_shape(value.nodes, value.node_count, AYNI_VALUE_NODE_COUNT_MAX)) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t bytes = 0;
    for (uint32_t at = 0; at < value.node_count; ++at) {
        const auto &node = value.nodes[at];
        if (node.reserved != 0U || (node.kind == AYNI_VALUE_BOOL && node.value.boolean > 1U) ||
            (node.kind == AYNI_VALUE_FLOAT64 && !finite_real(node.value.approximate)) ||
            (node.kind == AYNI_VALUE_DECIMAL &&
             (node.value.decimal.reserved != 0U ||
              node.value.decimal.scale > AYNI_DECIMAL_SCALE_MAX))) {
            return AYNI_INVALID_ARGUMENT;
        }
        const bool is_text = node.kind == AYNI_VALUE_TEXT || node.kind == AYNI_VALUE_ENUM ||
                             node.kind == AYNI_VALUE_IDENTIFIER;
        if (is_text && node.value.text.size > AYNI_VALUE_BYTES_MAX - bytes) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        if (is_text) {
            bytes += node.value.text.size;
        }
        if (is_text && (!text_valid(node.value.text) || (node.kind == AYNI_VALUE_IDENTIFIER &&
                                                         !CatalogId::is_valid(node.value.text)))) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (node.kind == AYNI_VALUE_LIST) {
            uint32_t count = 0;
            for (uint32_t child = at + 1U; child < node.subtree_end;
                 child = value.nodes[child].subtree_end) {
                ++count;
            }
            if (count > AYNI_VALUE_LIST_COUNT_MAX) {
                return AYNI_CAPACITY_EXCEEDED;
            }
        }
    }
    return AYNI_OK;
}

bool scalar_matches(const ayni_schema_node_v1 &schema, const ayni_value_node_v1 &value) {
    if (value.kind == AYNI_VALUE_NULL) {
        return (schema.flags & AYNI_SCHEMA_NULLABLE) != 0U;
    }
    if (schema.kind != value.kind) {
        return false;
    }
    switch (value.kind) {
    case AYNI_VALUE_BOOL:
        return true;
    case AYNI_VALUE_I64:
        return value.value.integer >= schema.min_i64 && value.value.integer <= schema.max_i64;
    case AYNI_VALUE_U64:
        return value.value.unsigned_integer >= schema.min_u64 &&
               value.value.unsigned_integer <= schema.max_u64;
    case AYNI_VALUE_FLOAT64:
        return value.value.approximate >= schema.min_float64 &&
               value.value.approximate <= schema.max_float64;
    case AYNI_VALUE_DECIMAL:
        return value.value.decimal.scale == schema.scale &&
               value.value.decimal.coefficient >= schema.min_i64 &&
               value.value.decimal.coefficient <= schema.max_i64;
    case AYNI_VALUE_TEXT:
        return value.value.text.size >= schema.min_count &&
               value.value.text.size <= schema.max_count;
    case AYNI_VALUE_ENUM:
    case AYNI_VALUE_IDENTIFIER:
        for (uint32_t at = 0; at < schema.choice_count; ++at) {
            if (same_text(value.value.text, schema.choices[at])) {
                return true;
            }
        }
        return schema.kind == AYNI_VALUE_IDENTIFIER && schema.choice_count == 0U;
    default:
        return false;
    }
}

ayni_status_t validate_schema(const ayni_schema_v1 &schema) {
    if (!tree_shape(schema.nodes, schema.node_count, AYNI_SCHEMA_NODE_COUNT_MAX)) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < schema.node_count; ++at) {
        if (!declaration_valid(schema, at)) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    return validate_schema_defaults(schema);
}
} // namespace ayni::catalog

extern "C" {
ayni_status_t ayni_value_validate(const ayni_value_v1 *value) {
    return value == nullptr ? AYNI_INVALID_ARGUMENT : ayni::catalog::validate_value(*value);
}
ayni_status_t ayni_schema_validate(const ayni_schema_v1 *schema) {
    return schema == nullptr ? AYNI_INVALID_ARGUMENT : ayni::catalog::validate_schema(*schema);
}

// Accumulate magnitude against the final sign-specific bound so INT64_MIN is
// accepted exactly, without signed overflow or a floating-point intermediate.
ayni_status_t ayni_decimal_parse(ayni_string_v1 text, uint32_t scale, ayni_decimal_v1 *out) {
    if (out == nullptr || text.data == nullptr || text.size == 0U ||
        scale > AYNI_DECIMAL_SCALE_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    const bool negative = text.data[0] == '-';
    uint32_t index = negative ? 1U : 0U;
    if (index == text.size) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto signed_max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    const uint64_t limit = signed_max + (negative ? 1U : 0U);
    uint64_t magnitude = 0;
    uint32_t fraction = 0;
    bool point = false;
    bool digit_seen = false;
    for (; index < text.size; ++index) {
        const char character = text.data[index];
        if (character == '.' && !point && digit_seen && index + 1U < text.size) {
            point = true;
            continue;
        }
        if (character < '0' || character > '9') {
            return AYNI_INVALID_ARGUMENT;
        }
        digit_seen = true;
        const auto digit = static_cast<uint64_t>(character - '0');
        if (point && fraction >= scale) {
            if (digit != 0U) {
                return AYNI_INVALID_ARGUMENT;
            }
            continue;
        }
        if (magnitude > (limit - digit) / ayni::catalog::decimal_radix) {
            return AYNI_INVALID_ARGUMENT;
        }
        magnitude = magnitude * ayni::catalog::decimal_radix + digit;
        if (point) {
            ++fraction;
        }
    }
    for (; fraction < scale; ++fraction) {
        if (magnitude > limit / ayni::catalog::decimal_radix) {
            return AYNI_INVALID_ARGUMENT;
        }
        magnitude *= ayni::catalog::decimal_radix;
    }
    const int64_t coefficient = negative && magnitude == signed_max + 1U
                                    ? std::numeric_limits<int64_t>::min()
                                : negative ? -static_cast<int64_t>(magnitude)
                                           : static_cast<int64_t>(magnitude);
    *out = {coefficient, scale, 0};
    return AYNI_OK;
}
} // extern "C"
