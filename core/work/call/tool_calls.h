// Typed tool-call lists of a TOOL_CALLS model result (ADR-0054). One byte
// format, tool-call list format 2 in contracts/storage/records.md, travels
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
enum class CallResultKind : uint32_t { text = 1, tool_calls, value, unavailable };

// One decoded proposal; every view borrows the immutable complete list body.
struct ToolCallView {
    ayni_string_v1 binding_id{};  // Exact trusted Binding identity, never a model alias.
    uint32_t binding_version = 0; // Positive in an admitted proposal.
    uint32_t input_kind = 0;      // VALUE or ARGUMENTS after admission; zero only before reading.
    ayni_string_v1 protocol_id{}; // Opaque external function-call identity.
    ayni_bytes_v1
        input{}; // Portable source tree, interpreted against the original Schema by owner.
};

// A complete decoded list, borrowing one immutable body. Decode once when a
// consumer needs every call; there is no per-index repeat of full-list admission.
struct ToolCallList {
    std::array<ToolCallView, AYNI_CALL_TOOL_COUNT_MAX> calls{};
    uint32_t count = 0; // Complete valid prefix after successful read; never partial.
};
bool read_tool_calls(ayni_bytes_v1 body, ToolCallList &out);

// Validates a Provider's list and measures its encoding: 1..CALL_TOOL_COUNT_MAX
// exact-size calls, catalog ids, unique opaque protocol IDs and valid source trees.
// This does not normalize business field order. Failure preserves size: INVALID_ARGUMENT,
// UNSUPPORTED_VERSION for a larger struct, or CAPACITY_EXCEEDED for text beyond the format bound.
ayni_status_t measure_tool_calls(const ayni_tool_call_v1 *calls, uint32_t count, uint32_t &size);
// Writes a list that passed measure_tool_calls with this size into size
// writable bytes, preserving the source trees. Validates it again
// and writes only zeros, never past size, if it changed since it was measured.
void write_tool_calls(const ayni_tool_call_v1 *calls, uint32_t count, uint8_t *out, uint32_t size);
// Strict decode of a whole list, with the measure rules and unique protocol
// identities; its call count, or 0 when the body is not a valid list.
uint32_t tool_call_count(ayni_bytes_v1 body);
// Call index of a valid list; false, preserving out, past its end.
bool read_tool_call(ayni_bytes_v1 body, uint32_t index, ToolCallView &out);

} // namespace ayni::work
#endif
