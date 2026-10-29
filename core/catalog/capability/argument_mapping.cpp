#include "catalog/capability/argument_mapping.h"
#include "catalog/catalog_id.h"
#include "foundation/utf8.h"

#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>

namespace ayni::catalog {
namespace {
constexpr int64_t exact_json_integer = AYNI_ARGUMENT_SAFE_INTEGER_MAX;
constexpr uint32_t decimal_radix = 10;
constexpr uint32_t no_node = UINT32_MAX;

bool digit(char value) {
    return value >= '0' && value <= '9';
}

// The adapter retains the original number spelling. Only JSON's finite lexical
// grammar is admitted; interpretation/rounding happens after Schema selection.
bool numeric_lexeme(ayni_string_v1 text) {
    if (text.size == 0U || text.data == nullptr) {
        return false;
    }
    uint32_t at = text.data[0] == '-' ? 1U : 0U;
    if (at == text.size || !digit(text.data[at])) {
        return false;
    }
    if (text.data[at] == '0') {
        ++at;
    } else {
        while (at < text.size && digit(text.data[at])) {
            ++at;
        }
    }
    if (at < text.size && text.data[at] == '.') {
        ++at;
        const uint32_t first = at;
        while (at < text.size && digit(text.data[at])) {
            ++at;
        }
        if (at == first) {
            return false;
        }
    }
    if (at < text.size && (text.data[at] == 'e' || text.data[at] == 'E')) {
        ++at;
        if (at < text.size && (text.data[at] == '+' || text.data[at] == '-')) {
            ++at;
        }
        const uint32_t first = at;
        while (at < text.size && digit(text.data[at])) {
            ++at;
        }
        if (at == first) {
            return false;
        }
    }
    return at == text.size;
}

bool same_name(ayni_string_v1 a, ayni_string_v1 b) {
    return a.size == b.size && (a.size == 0U || std::memcmp(a.data, b.data, a.size) == 0);
}

// One representation projection is shared by raw admission and already-decoded
// adapter values. It reads only the source tag's active member.
bool copy_source_node(const ayni_argument_node_v1 &source, uint32_t at, ayni_value_node_v1 &node) {
    node = {};
    node.field_id = source.name.size == 0U ? 0U : at + 1U;
    node.subtree_end = source.subtree_end;
    node.reserved = source.reserved;
    switch (source.kind) {
    case AYNI_ARGUMENT_NULL:
        node.kind = AYNI_VALUE_NULL;
        break;
    case AYNI_ARGUMENT_OBJECT:
        node.kind = AYNI_VALUE_OBJECT;
        break;
    case AYNI_ARGUMENT_LIST:
        node.kind = AYNI_VALUE_LIST;
        break;
    case AYNI_ARGUMENT_BOOL:
        node.kind = AYNI_VALUE_BOOL;
        node.value.boolean = source.value.boolean;
        break;
    case AYNI_ARGUMENT_NUMBER:
    case AYNI_ARGUMENT_STRING:
        node.kind = AYNI_VALUE_TEXT;
        node.value.text = source.value.text;
        break;
    default:
        return false;
    }
    return true;
}

// Reuse the value owner's sole structural walk. Temporary object IDs are local
// positions; semantic field IDs are assigned only after this shape is admitted.
ayni_status_t source_shape(const ayni_arguments_v1 &input, DecodedValue &shape) {
    if (input.nodes == nullptr || input.node_count == 0U || input.node_count > shape.nodes.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    shape.count = input.node_count;
    uint32_t bytes = 0;
    for (uint32_t at = 0; at < input.node_count; ++at) {
        const auto &source = input.nodes[at];
        if (source.name.size > AYNI_SCHEMA_NAME_BYTES_MAX ||
            (source.name.size != 0U && source.name.data == nullptr) ||
            !foundation::valid_utf8(source.name.data, source.name.size) ||
            source.name.size > AYNI_VALUE_BYTES_MAX - bytes) {
            return AYNI_INVALID_ARGUMENT;
        }
        bytes += source.name.size;
        if (source.kind == AYNI_ARGUMENT_NUMBER || source.kind == AYNI_ARGUMENT_STRING) {
            if (source.value.text.size > AYNI_VALUE_BYTES_MAX - bytes) {
                return AYNI_CAPACITY_EXCEEDED;
            }
            bytes += source.value.text.size;
        }
        if (source.kind == AYNI_ARGUMENT_NUMBER && !numeric_lexeme(source.value.text)) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (!copy_source_node(source, at, shape.nodes[at])) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    return validate_value(shape.view());
}

bool integer_spelling(ayni_string_v1 text) {
    if (text.size == 0U || text.data == nullptr) {
        return false;
    }
    uint32_t at = text.data[0] == '-' ? 1U : 0U;
    if (at == text.size || (text.data[at] == '0' && at + 1U != text.size)) {
        return false;
    }
    for (; at < text.size; ++at) {
        if (!digit(text.data[at])) {
            return false;
        }
    }
    return true;
}

template <typename Integer> bool parse_integer(ayni_string_v1 text, Integer &out) {
    if (!integer_spelling(text)) {
        return false;
    }
    const auto parsed = std::from_chars(text.data, text.data + text.size, out, decimal_radix);
    return parsed.ec == std::errc{} && parsed.ptr == text.data + text.size;
}

bool parse_real(ayni_string_v1 text, double &out) {
    const auto parsed =
        std::from_chars(text.data, text.data + text.size, out, std::chars_format::general);
    constexpr double finite = std::numeric_limits<double>::max();
    if (parsed.ec != std::errc{} || parsed.ptr != text.data + text.size || out < -finite ||
        out > finite) {
        return false;
    }
    if (out != 0.0) {
        return true;
    }
    // A nonzero significand may not silently underflow to zero. Digits in the
    // exponent do not turn an exact zero into a nonzero input (ADR-0084).
    for (uint32_t at = 0; at < text.size && text.data[at] != 'e' && text.data[at] != 'E'; ++at) {
        if (text.data[at] >= '1' && text.data[at] <= '9') {
            return false;
        }
    }
    return true;
}

// Only declared approximate numbers pass through binary64. Exact decimals and
// all integers use their original spelling and checked integer arithmetic.
bool resolve_scalar(const ayni_schema_node_v1 &schema, const ayni_argument_node_v1 &source,
                    ayni_value_node_v1 &node) {
    if (source.kind == AYNI_ARGUMENT_NULL) {
        return true;
    }
    if (value_container(schema.kind) || schema.kind == AYNI_VALUE_BOOL) {
        return node.kind == schema.kind;
    }
    const bool string = source.kind == AYNI_ARGUMENT_STRING;
    const bool number = source.kind == AYNI_ARGUMENT_NUMBER;
    if (!string && !number) {
        return false;
    }
    const auto text = source.value.text;
    node.kind = schema.kind;
    switch (schema.kind) {
    case AYNI_VALUE_I64: {
        const bool wide =
            schema.min_i64 < -exact_json_integer || schema.max_i64 > exact_json_integer;
        int64_t parsed = 0;
        if (!(wide ? string : number) || !parse_integer(text, parsed)) {
            return false;
        }
        node.value.integer = parsed;
        return true;
    }
    case AYNI_VALUE_U64: {
        uint64_t parsed = 0;
        if (!(schema.max_u64 > static_cast<uint64_t>(exact_json_integer) ? string : number) ||
            !parse_integer(text, parsed)) {
            return false;
        }
        node.value.unsigned_integer = parsed;
        return true;
    }
    case AYNI_VALUE_FLOAT64: {
        double parsed = 0.0;
        if (!number || !parse_real(text, parsed)) {
            return false;
        }
        node.value.approximate = parsed;
        return true;
    }
    case AYNI_VALUE_DECIMAL: {
        ayni_decimal_v1 parsed{};
        if (!string || ayni_decimal_parse(text, schema.scale, &parsed) != AYNI_OK) {
            return false;
        }
        node.value.decimal = parsed;
        return true;
    }
    case AYNI_VALUE_IDENTIFIER:
        return string && CatalogId::is_valid(text);
    case AYNI_VALUE_ENUM:
    case AYNI_VALUE_TEXT:
        return string;
    default:
        return false;
    }
}

uint32_t named_field(const ayni_schema_v1 &schema, uint32_t parent, ayni_string_v1 name) {
    for (uint32_t at = parent + 1U; at < schema.nodes[parent].subtree_end;
         at = schema.nodes[at].subtree_end) {
        if (same_name(schema.nodes[at].name, name)) {
            return at;
        }
    }
    return no_node;
}

// Shape intervals are already admitted. This walk resolves names once and
// rejects duplicate resolved IDs before the common default/order planner runs.
ayni_status_t resolve_names(const ayni_schema_v1 &schema, const ayni_arguments_v1 &input,
                            DecodedValue &candidate) {
    std::array<uint32_t, AYNI_VALUE_NODE_COUNT_MAX> schema_at{};
    std::array<uint32_t, AYNI_VALUE_DEPTH_MAX> parents{};
    uint32_t depth = 0;
    for (uint32_t at = 0; at < input.node_count; ++at) {
        while (depth != 0U && candidate.nodes[parents[depth - 1U]].subtree_end == at) {
            --depth;
        }
        uint32_t selected = 0;
        if (at != 0U) {
            const uint32_t parent = parents[depth - 1U];
            const uint32_t parent_schema = schema_at[parent];
            if (schema.nodes[parent_schema].kind == AYNI_VALUE_OBJECT) {
                selected = named_field(schema, parent_schema, input.nodes[at].name);
                if (selected == no_node) {
                    return AYNI_INVALID_ARGUMENT;
                }
                const uint32_t field = schema.nodes[selected].field_id;
                for (uint32_t before = parent + 1U; before < at;
                     before = candidate.nodes[before].subtree_end) {
                    if (candidate.nodes[before].field_id == field) {
                        return AYNI_INVALID_ARGUMENT;
                    }
                }
                candidate.nodes[at].field_id = field;
            } else if (schema.nodes[parent_schema].kind == AYNI_VALUE_LIST) {
                selected = parent_schema + 1U;
            } else {
                return AYNI_INVALID_ARGUMENT;
            }
        }
        schema_at[at] = selected;
        if (!resolve_scalar(schema.nodes[selected], input.nodes[at], candidate.nodes[at])) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (value_container(candidate.nodes[at].kind)) {
            parents[depth++] = at;
        }
    }
    return AYNI_OK;
}
} // namespace

ayni_status_t validate_arguments(const ayni_arguments_v1 &input) {
    DecodedValue shape;
    return source_shape(input, shape);
}

ayni_status_t map_arguments(const ayni_schema_v1 &schema, const ayni_arguments_v1 &input,
                            DecodedValue &out) {
    DecodedValue candidate;
    const auto shape = source_shape(input, candidate);
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto resolved = resolve_names(schema, input, candidate);
    if (resolved == AYNI_OK) {
        out = candidate;
    }
    return resolved;
}
ayni_status_t map_admitted_arguments(const ayni_schema_v1 &schema, const ayni_arguments_v1 &input,
                                     DecodedValue &out) {
    DecodedValue candidate;
    candidate.count = input.node_count;
    for (uint32_t at = 0; at < input.node_count; ++at) {
        if (!copy_source_node(input.nodes[at], at, candidate.nodes[at])) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    const auto resolved = resolve_names(schema, input, candidate);
    if (resolved == AYNI_OK) {
        out = candidate;
    }
    return resolved;
}

} // namespace ayni::catalog

extern "C" ayni_status_t ayni_arguments_validate(const ayni_arguments_v1 *input) {
    return input == nullptr ? AYNI_INVALID_ARGUMENT : ayni::catalog::validate_arguments(*input);
}

extern "C" ayni_status_t ayni_arguments_normalize(const ayni_schema_v1 *schema,
                                                  const ayni_arguments_v1 *input,
                                                  ayni_value_buffer_v1 *output) {
    if (schema == nullptr || input == nullptr || output == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto status = ayni::catalog::validate_schema(*schema);
    if (status != AYNI_OK) {
        return status;
    }
    ayni::catalog::DecodedValue mapped;
    status = ayni::catalog::map_arguments(*schema, *input, mapped);
    return status == AYNI_OK ? ayni::catalog::normalize_value(*schema, mapped.view(), *output)
                             : status;
}
