// Shared bounded JSON edge codec; no Kernel state, I/O, JSON DOM or allocation.
#ifndef AYNI_VALUES_JSON_HPP
#define AYNI_VALUES_JSON_HPP
#include <ayni/arguments.h>
#include <charconv>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>
namespace ayni::json {
// Borrows one exclusive mutable output lane; owns only its cursor/error. No
// allocation or I/O. Strings/trees must be immutable UTF-8 and must not alias
// output storage. Since 0.48.
class Writer {
  public:
    // Capacity is bytes; null is valid only for a lane that cannot accept bytes.
    Writer(char *buffer, size_t capacity) : data_(buffer), capacity_(capacity) {}
    // Count the exact escaped encoding without an output lane. Since 0.50.
    static Writer measuring() { return Writer(Measure{}); }
    // One cursor owns the append position; cloning it would share mutable state.
    Writer(const Writer &) = delete;
    // Same exclusivity rule as construction.
    Writer &operator=(const Writer &) = delete;
    // Append already-encoded JSON syntax; the caller owns structural framing.
    void raw(std::string_view value) {
        if (!ok_) {
            return;
        }
        if (value.size() > capacity_ - used_ || (value.size() && !data_ && !measuring_)) {
            ok_ = false;
            return;
        }
        if (value.size() && data_) {
            std::memcpy(data_ + used_, value.data(), value.size());
        }
        used_ += value.size();
        constexpr unsigned continuation_mask = 0xc0, continuation_bits = 0x80;
        for (size_t at = 0; at < value.size(); ++at) {
            if ((static_cast<unsigned char>(value[at]) & continuation_mask) != continuation_bits) {
                ++characters_;
            }
        }
    }
    // Unicode code points in the emitted JSON representation, including syntax and escapes.
    size_t characters() const { return characters_; }
    // Quote a borrowed UTF-8 string, including embedded zero bytes.
    void string(ayni_string_v1 value) {
        if (value.size && !value.data) {
            ok_ = false;
            return;
        }
        raw("\"");
        escaped(value);
        raw("\"");
    }
    // Emit only the escaped body, for a description composed of bounded views.
    void escaped(ayni_string_v1 value) {
        if (!ok_ || (value.size && !value.data)) {
            ok_ = false;
            return;
        }
        constexpr char hex[] = "0123456789abcdef";
        constexpr unsigned nibble_bits = 4;
        constexpr unsigned nibble_mask = 15;
        for (uint32_t at = 0; at < value.size && ok_; ++at) {
            const auto byte = static_cast<unsigned char>(value.data[at]);
            if (byte == '"' || byte == '\\') {
                raw("\\");
                raw({value.data + at, 1});
            } else if (byte < ' ') {
                const char escape[] = {
                    '\\', 'u', '0', '0', hex[byte >> nibble_bits], hex[byte & nibble_mask]};
                raw({escape, sizeof(escape)});
            } else {
                raw({value.data + at, 1});
            }
        }
    }
    // Same quoting operation for C++ borrowed text; reject an unrepresentable byte count.
    void string(std::string_view value) {
        if (value.size() > UINT32_MAX) {
            ok_ = false;
            return;
        }
        string(ayni_string_v1{value.data(), static_cast<uint32_t>(value.size())});
    }
    // Emit exact signed integer digits; Schema decides whether JSON needs quotes.
    void number(int64_t value) {
        char buffer[integer_format_bytes];
        const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (written.ec != std::errc{}) {
            ok_ = false;
            return;
        }
        raw({buffer, static_cast<size_t>(written.ptr - buffer)});
    }
    // Emit the complete unsigned range without conversion to a signed or floating type.
    void unsigned_number(uint64_t value) {
        char buffer[integer_format_bytes];
        const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
        if (written.ec != std::errc{}) {
            ok_ = false;
            return;
        }
        raw({buffer, static_cast<size_t>(written.ptr - buffer)});
    }
    // Emit finite binary64 with enough decimal digits for a round trip.
    void real(double value) {
        constexpr auto maximum = std::numeric_limits<double>::max();
        if (!(value >= -maximum && value <= maximum)) {
            ok_ = false;
            return;
        }
        constexpr size_t binary64_bytes = 64;
        char buffer[binary64_bytes];
        const auto written =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                          std::numeric_limits<double>::max_digits10);
        if (written.ec != std::errc{}) {
            ok_ = false;
            return;
        }
        raw({buffer, static_cast<size_t>(written.ptr - buffer)});
    }
    // Reports the used prefix; false requires discarding it, never truncating a message.
    bool finish(size_t &size) const {
        size = used_;
        return ok_;
    }

  private:
    struct Measure {};
    explicit Writer(Measure) : data_(nullptr), capacity_(SIZE_MAX), measuring_(true) {}
    static constexpr size_t integer_format_bytes = sizeof("-18446744073709551615") - 1;
    char *data_;             // Caller-owned output, borrowed until the last append.
    size_t capacity_;        // Byte capacity; used_ never exceeds it.
    size_t used_ = 0;        // Written prefix bytes; zero before the first append.
    size_t characters_ = 0;  // Serialized Unicode characters; <=used_, not a model token count.
    bool ok_ = true;         // Sticky formatting/capacity status; no bytes append after failure.
    bool measuring_ = false; // Explicit counting sink; never a writable output lane.
};

namespace detail {
inline void argument(Writer &out, ayni_arguments_v1 input, uint32_t at) {
    const auto &node = input.nodes[at];
    switch (node.kind) {
    case AYNI_ARGUMENT_NULL:
        out.raw("null");
        break;
    case AYNI_ARGUMENT_BOOL:
        out.raw(node.value.boolean ? "true" : "false");
        break;
    case AYNI_ARGUMENT_NUMBER:
        out.raw({node.value.text.data, node.value.text.size});
        break;
    case AYNI_ARGUMENT_STRING:
        out.string(node.value.text);
        break;
    case AYNI_ARGUMENT_OBJECT:
    case AYNI_ARGUMENT_LIST: {
        const bool object = node.kind == AYNI_ARGUMENT_OBJECT;
        out.raw(object ? "{" : "[");
        bool first = true;
        for (uint32_t child = at + 1U; child < node.subtree_end;
             child = input.nodes[child].subtree_end) {
            if (!first) {
                out.raw(",");
            }
            first = false;
            if (object) {
                out.string(input.nodes[child].name);
                out.raw(":");
            }
            argument(out, input, child);
        }
        out.raw(object ? "}" : "]");
        break;
    }
    }
}
} // namespace detail
// Lossless adapter tree to canonical JSON; number lexemes are never rounded.
// Shape admission is shared with the portable source owner. Since 0.50.
inline ayni_status_t write_arguments(Writer &out, ayni_arguments_v1 input) {
    const auto status = ayni_arguments_validate(&input);
    if (status == AYNI_OK) {
        detail::argument(out, input, 0);
    }
    return status;
}

// The caller supplies immutable Schema/value trees already matched and
// normalized under that same original definition; at is a valid Schema index.
// These functions
// preserve exact decimals/wide integers as strings and use declared field names;
// they neither normalize defaults nor authorize execution. A failed finish
// means the caller must discard the partial buffer, never silently truncate it.
// write_value reports tree-kind/name mismatches; finish is still required for
// the streaming writer's capacity status and complete byte count.
inline void write_schema(Writer &out, ayni_schema_v1 schema, uint32_t at = 0);
inline ayni_status_t write_value(Writer &out, ayni_schema_v1 schema, ayni_value_v1 value);
namespace detail {
constexpr int64_t exact_json_integer = AYNI_ARGUMENT_SAFE_INTEGER_MAX;
inline bool requires_integer_string(const ayni_schema_node_v1 &node) {
    return (node.kind == AYNI_VALUE_I64 &&
            (node.min_i64 < -exact_json_integer || node.max_i64 > exact_json_integer)) ||
           (node.kind == AYNI_VALUE_U64 &&
            node.max_u64 > static_cast<uint64_t>(exact_json_integer));
}
} // namespace detail

// JSON Schema is an adapter projection of the original machine Schema. Numeric
// strings preserve wide integers and fixed-scale decimals; Core alone admits
// arguments/defaults and derives authority from the resulting native value.
namespace detail {
// Original field names and optionality stay with the Schema, independently of
// business value order. The complete declared object is emitted as one shape.
inline void object_schema(Writer &out, ayni_schema_v1 schema, uint32_t at) {
    const auto &node = schema.nodes[at];
    out.raw(",\"additionalProperties\":false,\"properties\":{");
    bool first = true;
    for (uint32_t child = at + 1; child < node.subtree_end;
         child = schema.nodes[child].subtree_end) {
        if (!first) {
            out.raw(",");
        }
        first = false;
        out.string(schema.nodes[child].name);
        out.raw(":");
        write_schema(out, schema, child);
    }
    out.raw("},\"required\":[");
    first = true;
    for (uint32_t child = at + 1; child < node.subtree_end;
         child = schema.nodes[child].subtree_end) {
        if (!(schema.nodes[child].flags & AYNI_SCHEMA_REQUIRED)) {
            continue;
        }
        if (!first) {
            out.raw(",");
        }
        first = false;
        out.string(schema.nodes[child].name);
    }
    out.raw("]");
}
inline void enum_schema(Writer &out, const ayni_schema_node_v1 &node) {
    out.raw(",\"enum\":[");
    for (uint32_t choice = 0; choice < node.choice_count; ++choice) {
        if (choice) {
            out.raw(",");
        }
        out.string(node.choices[choice]);
    }
    if (node.flags & AYNI_SCHEMA_NULLABLE) {
        out.raw(",null");
    }
    out.raw("]");
}
// Numeric strings carry exact bounds/units in their description; JSON numbers
// carry machine bounds. UTF-8 byte counts must not become Unicode lengths.
inline void scalar_schema(Writer &out, const ayni_schema_node_v1 &node) {
    const bool integer_string = requires_integer_string(node);
    if (node.kind == AYNI_VALUE_TEXT) {
        out.raw(",\"description\":\"UTF-8 maximum ");
        out.number(node.max_count);
        out.raw(" bytes; minimum ");
        out.number(node.min_count);
        out.raw(" UTF-8 bytes\"");
    } else if (node.kind == AYNI_VALUE_DECIMAL) {
        out.raw(",\"description\":\"Exact decimal string; no exponent or rounding. Scale ");
        out.number(node.scale);
        out.raw(". Exact coefficient range [");
        out.number(node.min_i64);
        out.raw(", ");
        out.number(node.max_i64);
        out.raw("]; value = coefficient * 10^-scale. Unit: ");
        out.escaped(node.unit);
        out.raw("\"");
    } else if (integer_string) {
        out.raw(
            ",\"description\":\"Exact integer string; no exponent or fraction. Inclusive range [");
        if (node.kind == AYNI_VALUE_I64) {
            out.number(node.min_i64);
            out.raw(", ");
            out.number(node.max_i64);
        } else {
            out.unsigned_number(node.min_u64);
            out.raw(", ");
            out.unsigned_number(node.max_u64);
        }
        out.raw("]. Unit: ");
        out.escaped(node.unit);
        out.raw("\"");
    } else if (node.kind == AYNI_VALUE_FLOAT64) {
        out.raw(",\"minimum\":");
        out.real(node.min_float64);
        out.raw(",\"maximum\":");
        out.real(node.max_float64);
        out.raw(",\"description\":\"Finite approximate binary64. Unit: ");
        out.escaped(node.unit);
        out.raw("\"");
    } else if (node.kind == AYNI_VALUE_IDENTIFIER) {
        out.raw(",\"minLength\":1,\"maxLength\":");
        out.number(AYNI_CATALOG_ID_MAX_BYTES);
        out.raw(",\"pattern\":\"^[a-z0-9_.-]+$\"");
    } else if (node.kind == AYNI_VALUE_I64 || node.kind == AYNI_VALUE_U64) {
        out.raw(",\"minimum\":");
        out.number(node.kind == AYNI_VALUE_I64 ? node.min_i64 : static_cast<int64_t>(node.min_u64));
        out.raw(",\"maximum\":");
        out.number(node.kind == AYNI_VALUE_I64 ? node.max_i64 : static_cast<int64_t>(node.max_u64));
    }
}
} // namespace detail
inline void write_schema(Writer &out, ayni_schema_v1 schema, uint32_t at) {
    const auto &node = schema.nodes[at];
    const bool integer_string = detail::requires_integer_string(node);
    const char *kind = "string";
    switch (node.kind) {
    case AYNI_VALUE_OBJECT:
        kind = "object";
        break;
    case AYNI_VALUE_LIST:
        kind = "array";
        break;
    case AYNI_VALUE_BOOL:
        kind = "boolean";
        break;
    case AYNI_VALUE_I64:
    case AYNI_VALUE_U64:
        kind = integer_string ? "string" : "integer";
        break;
    case AYNI_VALUE_FLOAT64:
        kind = "number";
        break;
    default:
        break;
    }
    out.raw("{\"type\":");
    if (node.flags & AYNI_SCHEMA_NULLABLE) {
        out.raw("[");
    }
    out.string(ayni_string_v1{kind, static_cast<uint32_t>(std::strlen(kind))});
    if (node.flags & AYNI_SCHEMA_NULLABLE) {
        out.raw(",\"null\"]");
    }
    if (node.kind == AYNI_VALUE_OBJECT) {
        detail::object_schema(out, schema, at);
    } else if (node.kind == AYNI_VALUE_LIST) {
        out.raw(",\"minItems\":");
        out.number(node.min_count);
        out.raw(",\"maxItems\":");
        out.number(node.max_count);
        out.raw(",\"items\":");
        write_schema(out, schema, at + 1);
    } else if ((node.kind == AYNI_VALUE_ENUM || node.kind == AYNI_VALUE_IDENTIFIER) &&
               node.choice_count) {
        detail::enum_schema(out, node);
    } else {
        detail::scalar_schema(out, node);
    }
    out.raw("}");
}

namespace detail {
// Fixed-scale decimals never pass through a floating number or a locale. The
// unsigned magnitude calculation also represents INT64_MIN without overflow.
inline ayni_status_t decimal_json(Writer &out, ayni_decimal_v1 value) {
    constexpr uint32_t decimal_digit_bytes = std::numeric_limits<uint64_t>::digits10 + 1;
    char digits[decimal_digit_bytes];
    const uint64_t magnitude = value.coefficient < 0
                                   ? static_cast<uint64_t>(-(value.coefficient + 1)) + 1U
                                   : static_cast<uint64_t>(value.coefficient);
    const auto written = std::to_chars(digits, digits + sizeof(digits), magnitude);
    if (written.ec != std::errc{}) {
        return AYNI_INTERNAL;
    }
    const auto count = static_cast<uint32_t>(written.ptr - digits);
    out.raw("\"");
    if (value.coefficient < 0) {
        out.raw("-");
    }
    if (value.scale == 0) {
        out.raw({digits, count});
    } else if (count > value.scale) {
        const auto whole = count - value.scale;
        out.raw({digits, whole});
        out.raw(".");
        out.raw({digits + whole, value.scale});
    } else {
        out.raw("0.");
        for (uint32_t at = count; at < value.scale; ++at) {
            out.raw("0");
        }
        out.raw({digits, count});
    }
    out.raw("\"");
    return AYNI_OK;
}

// Both trees are admitted and immutable. Stable business IDs resolve to the
// original Schema's names, list order/repetitions are preserved, and only the
// active native scalar representation is read (ADR-0084).
inline ayni_status_t value_node(Writer &out, ayni_schema_v1 schema, uint32_t schema_at,
                                ayni_value_v1 value, uint32_t value_at);
// Resolve native IDs against the original object Schema, rejecting an unknown
// field rather than inventing a name. Child errors preserve the classified cause.
inline ayni_status_t object_value(Writer &out, ayni_schema_v1 schema, uint32_t schema_at,
                                  ayni_value_v1 value, uint32_t value_at) {
    const auto &node = value.nodes[value_at];
    const auto &definition = schema.nodes[schema_at];
    out.raw("{");
    bool first = true;
    for (uint32_t child = value_at + 1; child < node.subtree_end;
         child = value.nodes[child].subtree_end) {
        uint32_t field = schema_at + 1;
        while (field < definition.subtree_end &&
               schema.nodes[field].field_id != value.nodes[child].field_id) {
            field = schema.nodes[field].subtree_end;
        }
        if (field == definition.subtree_end) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (!first) {
            out.raw(",");
        }
        first = false;
        out.string(schema.nodes[field].name);
        out.raw(":");
        const auto status = value_node(out, schema, field, value, child);
        if (status != AYNI_OK) {
            return status;
        }
    }
    out.raw("}");
    return AYNI_OK;
}
// Ordered/repeated items remain separate entries; the one item Schema applies
// to every row and never merges independently authorized data.
inline ayni_status_t list_value(Writer &out, ayni_schema_v1 schema, uint32_t schema_at,
                                ayni_value_v1 value, uint32_t value_at) {
    const auto &node = value.nodes[value_at];
    out.raw("[");
    bool first = true;
    for (uint32_t child = value_at + 1; child < node.subtree_end;
         child = value.nodes[child].subtree_end) {
        if (!first) {
            out.raw(",");
        }
        first = false;
        const auto status = value_node(out, schema, schema_at + 1, value, child);
        if (status != AYNI_OK) {
            return status;
        }
    }
    out.raw("]");
    return AYNI_OK;
}
inline ayni_status_t value_node(Writer &out, ayni_schema_v1 schema, uint32_t schema_at,
                                ayni_value_v1 value, uint32_t value_at) {
    const auto &node = value.nodes[value_at];
    const auto &definition = schema.nodes[schema_at];
    if (node.kind == AYNI_VALUE_NULL) {
        if (!(definition.flags & AYNI_SCHEMA_NULLABLE)) {
            return AYNI_INVALID_ARGUMENT;
        }
        out.raw("null");
        return AYNI_OK;
    }
    if (node.kind != definition.kind) {
        return AYNI_INVALID_ARGUMENT;
    }
    switch (node.kind) {
    case AYNI_VALUE_OBJECT:
        return object_value(out, schema, schema_at, value, value_at);
    case AYNI_VALUE_LIST:
        return list_value(out, schema, schema_at, value, value_at);
    case AYNI_VALUE_BOOL:
        out.raw(node.value.boolean ? "true" : "false");
        return AYNI_OK;
    case AYNI_VALUE_I64:
        if (requires_integer_string(definition)) {
            out.raw("\"");
            out.number(node.value.integer);
            out.raw("\"");
        } else {
            out.number(node.value.integer);
        }
        return AYNI_OK;
    case AYNI_VALUE_U64:
        if (requires_integer_string(definition)) {
            out.raw("\"");
            out.unsigned_number(node.value.unsigned_integer);
            out.raw("\"");
        } else {
            out.unsigned_number(node.value.unsigned_integer);
        }
        return AYNI_OK;
    case AYNI_VALUE_FLOAT64:
        out.real(node.value.approximate);
        return AYNI_OK;
    case AYNI_VALUE_DECIMAL:
        return decimal_json(out, node.value.decimal);
    case AYNI_VALUE_TEXT:
    case AYNI_VALUE_ENUM:
    case AYNI_VALUE_IDENTIFIER:
        out.string(node.value.text);
        return AYNI_OK;
    default:
        return AYNI_INVALID_ARGUMENT;
    }
}
} // namespace detail

inline ayni_status_t write_value(Writer &out, ayni_schema_v1 schema, ayni_value_v1 value) {
    if (!schema.nodes || !schema.node_count || !value.nodes || !value.node_count) {
        return AYNI_INVALID_ARGUMENT;
    }
    return detail::value_node(out, schema, 0, value, 0);
}

} // namespace ayni::json
#endif
