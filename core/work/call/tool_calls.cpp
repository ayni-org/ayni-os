#include "work/call/tool_calls.h"

#include "authority/scope/scope.h"
#include "catalog/abi_checks.h"
#include "catalog/capability/call_contract.h"
#include "catalog/capability/field_domain.h"
#include "durability/batch/record_body.h"
#include "work/call/call_input.h"

#include <limits>

namespace ayni::work {
namespace {
// Tool-call list format 1, contracts/storage/records.md: a u32 count, then per
// call its version, id, fields and text; per field its id, kind, unit and value.
constexpr uint32_t list_header_size = 4;
constexpr uint32_t call_fixed_size = 16;  // Version and the id, field count and text lengths.
constexpr uint32_t field_fixed_size = 12; // Id, kind and unit length.
constexpr uint32_t integer_size = 8;
constexpr uint32_t token_length_size = 4;

// No implementation-defined unsigned-to-signed conversion, including INT64_MIN.
int64_t signed_value(uint64_t bits) {
    return bits <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
               ? static_cast<int64_t>(bits)
               : -1 - static_cast<int64_t>(~bits);
}

bool integer(ayni_authority_value_kind_t kind) {
    return kind == AYNI_AUTHORITY_I64;
}

// One ABI call's validated singleton values, by input index, and their id order.
struct CallValues {
    catalog::FieldOrder order{};
    std::array<catalog::FieldValues, AYNI_AUTHORITY_FIELD_COUNT_MAX> values{};
};

ayni_status_t read_call(const ayni_tool_call_v1 &call, CallValues &out, uint32_t &size) {
    auto status = catalog::check_struct_size(call.struct_size, sizeof(call), sizeof(call));
    if (status != AYNI_OK) {
        return status;
    }
    if (call.binding_version == 0U || !catalog::CatalogId::is_valid(call.binding_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    status = authority::check_scope_header(call.arguments);
    if (status != AYNI_OK) {
        return status;
    }
    uint32_t total = call_fixed_size + call.binding_id.size + call.text.size;
    for (uint32_t at = 0; at < call.arguments.field_count; ++at) {
        const auto &field = call.arguments.fields[at];
        status = authority::check_scope_field_header(field);
        if (status == AYNI_OK) {
            status = catalog::read_field_values(field.allowed, out.values[at]);
        }
        if (status != AYNI_OK) {
            return status;
        }
        const auto &values = out.values[at];
        if ((integer(values.kind) ? values.minimum != values.maximum : values.token_count != 1U) ||
            !catalog::insert_field_order(call.arguments.fields, at, out.order)) {
            return AYNI_INVALID_ARGUMENT;
        }
        total += field_fixed_size + values.unit.size +
                 (integer(values.kind) ? integer_size : token_length_size + values.tokens[0].size);
    }
    // The tool's own text limit is checked by the Kernel against its Binding.
    status = check_call_text(call.text, catalog::call_text_limit_max);
    if (status != AYNI_OK) {
        return status;
    }
    size = total;
    return AYNI_OK;
}

void write_call(const ayni_tool_call_v1 &call, const CallValues &values,
                durability::BodyWriter &writer) {
    writer.u32(call.binding_version);
    writer.bytes(call.binding_id.data, call.binding_id.size);
    writer.u32(call.arguments.field_count);
    for (uint32_t at = 0; at < call.arguments.field_count; ++at) {
        const uint32_t index = values.order[at];
        const auto &value = values.values[index];
        writer.u32(call.arguments.fields[index].field_id);
        writer.u32(value.kind);
        writer.bytes(value.unit.data, value.unit.size);
        if (integer(value.kind)) {
            writer.u64(static_cast<uint64_t>(value.minimum));
        } else {
            writer.bytes(value.tokens[0].data, value.tokens[0].size);
        }
    }
    writer.bytes(call.text.data, call.text.size);
}

ayni_string_v1 string(const uint8_t *data, uint32_t size) {
    return {reinterpret_cast<const char *>(data), size};
}

// Reads one call strictly; reader failure is checked by the caller at the end.
bool read_encoded(durability::BodyReader &reader, ToolCallView &out) {
    ToolCallView call{};
    uint32_t size = 0;
    call.binding_version = reader.u32();
    const auto *id = reader.bytes(catalog::CatalogId::max_size, size);
    call.binding_id = string(id, size);
    call.field_count = reader.u32();
    if (call.binding_version == 0U || !catalog::CatalogId::is_valid(call.binding_id) ||
        call.field_count > AYNI_AUTHORITY_FIELD_COUNT_MAX) {
        return false;
    }
    uint32_t previous = 0;
    for (uint32_t at = 0; at < call.field_count; ++at) {
        auto &field = call.fields[at];
        field.id = reader.u32();
        field.kind = reader.u32();
        const auto *unit = reader.bytes(catalog::CatalogId::max_size, size);
        field.unit = string(unit, size);
        if (integer(field.kind)) {
            field.integer = signed_value(reader.u64());
        } else {
            const auto *token = reader.bytes(catalog::CatalogId::max_size, size);
            field.token = string(token, size);
        }
        const bool valid =
            integer(field.kind)
                ? catalog::CatalogId::is_valid(field.unit)
                : (field.kind == AYNI_AUTHORITY_ENUM || field.kind == AYNI_AUTHORITY_IDENTIFIER) &&
                      field.unit.size == 0U && catalog::CatalogId::is_valid(field.token);
        if (field.id <= previous || !valid) {
            return false;
        }
        previous = field.id;
    }
    const auto *text = reader.bytes(catalog::call_text_limit_max, size);
    call.text = string(text, size);
    if (check_call_text(call.text, catalog::call_text_limit_max) != AYNI_OK) {
        return false;
    }
    out = call;
    return true;
}

// Decodes the whole list; selected, when present, receives call index.
uint32_t decode(ayni_bytes_v1 body, uint32_t index, ToolCallView *selected) {
    if (body.data == nullptr || body.size < list_header_size) {
        return 0;
    }
    durability::BodyReader reader(body.data, body.size);
    const uint32_t count = reader.u32();
    if (count == 0U || count > AYNI_CALL_TOOL_COUNT_MAX) {
        return 0;
    }
    ToolCallView chosen{};
    for (uint32_t at = 0; at < count; ++at) {
        ToolCallView call{};
        if (!read_encoded(reader, call)) {
            return 0;
        }
        if (at == index) {
            chosen = call;
        }
    }
    if (!reader.ok()) {
        return 0;
    }
    if (selected != nullptr && index < count) {
        *selected = chosen;
    }
    return count;
}
} // namespace

ayni_scope_v1 ToolCallView::scope(ayni_scope_field_v1 *out_fields,
                                  ayni_string_v1 *out_tokens) const {
    for (uint32_t at = 0; at < field_count; ++at) {
        const auto &field = fields[at];
        auto &out = out_fields[at];
        out = {};
        out.struct_size = sizeof(out);
        out.field_id = field.id;
        out.allowed.struct_size = sizeof(out.allowed);
        out.allowed.kind = field.kind;
        out.allowed.unit = field.unit;
        if (integer(field.kind)) {
            out.allowed.values.integer = {field.integer, field.integer};
        } else {
            out_tokens[at] = field.token;
            out.allowed.values.tokens = {&out_tokens[at], 1, 0};
        }
    }
    return {sizeof(ayni_scope_v1), field_count, out_fields};
}

ayni_status_t measure_tool_calls(const ayni_tool_call_v1 *calls, uint32_t count, uint32_t &size) {
    if (count == 0U || count > AYNI_CALL_TOOL_COUNT_MAX || calls == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    uint32_t total = list_header_size;
    for (uint32_t at = 0; at < count; ++at) {
        CallValues values;
        uint32_t call_size = 0;
        const auto status = read_call(calls[at], values, call_size);
        if (status != AYNI_OK) {
            return status;
        }
        total += call_size;
    }
    size = total;
    return AYNI_OK;
}

void write_tool_calls(const ayni_tool_call_v1 *calls, uint32_t count, uint8_t *out, uint32_t size) {
    durability::BodyWriter writer(out, size);
    writer.u32(count);
    for (uint32_t at = 0; at < count; ++at) {
        CallValues values;
        uint32_t call_size = 0;
        static_cast<void>(read_call(calls[at], values, call_size)); // Measured unchanged.
        write_call(calls[at], values, writer);
    }
}

uint32_t tool_call_count(ayni_bytes_v1 body) {
    return decode(body, 0, nullptr);
}

bool read_tool_call(ayni_bytes_v1 body, uint32_t index, ToolCallView &out) {
    return decode(body, index, &out) > index;
}
} // namespace ayni::work
