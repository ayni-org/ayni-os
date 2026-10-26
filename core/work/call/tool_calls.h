// Typed tool-call lists of a TOOL_CALLS model result (ADR-0054). One byte
// format, tool-call list format 1 in contracts/storage/records.md, travels
// through the Provider mailbox and stays in the call row; Core never sees
// model JSON. Shape only: whether a tool was offered and its values are
// authorized is the Kernel's check, and requesting one is the strategy's.
#ifndef AYNI_CORE_WORK_CALL_TOOL_CALLS_H
#define AYNI_CORE_WORK_CALL_TOOL_CALLS_H

#include <array>
#include <ayni/calls.h>
#include <cstdint>

namespace ayni::work {

// What a terminal result body holds. Only a TOOL_CALLS contract's completed
// result may hold tool calls; every other body is UTF-8 text.
enum class CallResultKind : uint32_t { text = 1, tool_calls };

// One decoded concrete value; views borrow the encoded list.
struct ToolCallField {
    uint32_t id;                      // Nonzero, ascending within a call.
    ayni_authority_value_kind_t kind; // One public kind.
    ayni_string_v1 unit;              // I64: a catalog id; tokens: empty.
    int64_t integer;                  // The I64 value; zero for tokens.
    ayni_string_v1 token;             // The ENUM/IDENTIFIER token; empty for I64.
};

// One decoded call of a list; views borrow the encoded list.
struct ToolCallView {
    ayni_string_v1 binding_id; // A catalog id.
    uint32_t binding_version;  // Nonzero.
    uint32_t field_count;      // Valid prefix of fields.
    std::array<ToolCallField, AYNI_AUTHORITY_FIELD_COUNT_MAX> fields;
    ayni_string_v1 text; // UTF-8 of at most catalog::call_text_limit_max bytes.

    // The typed arguments, built in caller storage of field_count entries
    // each; they borrow that storage and the encoded list.
    ayni_scope_v1 scope(ayni_scope_field_v1 *out_fields, ayni_string_v1 *out_tokens) const;
};

// Validates a Provider's list and measures its encoding: 1..CALL_TOOL_COUNT_MAX
// exact-size calls, catalog ids, every field once with one value, UTF-8 text.
// Field order is free. Failure preserves size: INVALID_ARGUMENT, UNSUPPORTED_VERSION
// for a larger struct, or CAPACITY_EXCEEDED for text beyond the format bound.
ayni_status_t measure_tool_calls(const ayni_tool_call_v1 *calls, uint32_t count, uint32_t &size);
// Writes a list that passed measure_tool_calls with this size into size
// writable bytes, each call's fields in ascending id order. Validates it again
// and writes only zeros, never past size, if it changed since it was measured.
void write_tool_calls(const ayni_tool_call_v1 *calls, uint32_t count, uint8_t *out, uint32_t size);
// Strict decode of a whole list, with the measure rules and canonical field
// order; its call count, or 0 when the body is not a valid list.
uint32_t tool_call_count(ayni_bytes_v1 body);
// Call index of a valid list; false, preserving out, past its end.
bool read_tool_call(ayni_bytes_v1 body, uint32_t index, ToolCallView &out);

} // namespace ayni::work
#endif
