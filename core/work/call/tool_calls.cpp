#include "work/call/tool_calls.h"
#include "foundation/workspace.h"

#include "catalog/abi_checks.h"
#include "catalog/capability/argument_encoding.h"
#include "catalog/capability/field_domain.h"
#include "catalog/capability/value_encoding.h"
#include "foundation/record_body.h"
#include "work/call/call_input.h"

#include <cstring>

namespace ayni::work {
namespace {
// Portable format 3: magic/version/count, then function name, input kind,
// opaque protocol ID and one length-prefixed value/arguments body (records.md).
constexpr uint32_t list_magic = 0x33435441U; // ATC3, distinct from every old list count.
constexpr uint32_t list_version = 3;
constexpr uint32_t list_header_size = 16;
constexpr uint32_t call_fixed_size = 16;

// Owns the admitted sizes and ABI views for one write. Payload storage is
// immutable and borrowed through the enclosing producer operation.
struct CallValues {
    ayni_tool_call_v1 source{};
    uint32_t payload_size = 0;
};
using ListPlan = std::array<CallValues, AYNI_CALL_TOOL_COUNT_MAX>;

// Producer admission checks source representation, never business Schema or
// authorization. Those decisions belong to the attempt's owner after copying.
ayni_status_t read_call(const ayni_tool_call_v1 &input, CallValues &out, uint32_t &size) {
    const auto call = input;
    auto status = catalog::check_struct_size(call.struct_size, sizeof(call), sizeof(call));
    if (status != AYNI_OK) {
        return status;
    }
    if (call.function_name.size == 0U || call.protocol_id.size == 0U ||
        check_call_text(call.function_name, AYNI_CATALOG_ID_MAX_BYTES) != AYNI_OK) {
        return AYNI_INVALID_ARGUMENT;
    }
    status = check_call_text(call.protocol_id, AYNI_MODEL_TOOL_ID_BYTES_MAX);
    if (status != AYNI_OK) {
        return status;
    }
    uint32_t payload_size = 0;
    if (call.input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
        status = catalog::encode_arguments(call.input.arguments, nullptr, 0, payload_size);
        if (status != AYNI_BUFFER_TOO_SMALL) {
            return status;
        }
    } else if (call.input_kind == AYNI_TOOL_INPUT_INVALID_ARGUMENTS) {
        status = check_call_text(call.input.invalid_arguments, AYNI_CALL_RESULT_BYTES_MAX);
        if (status != AYNI_OK) {
            return status;
        }
        payload_size = call.input.invalid_arguments.size;
    } else {
        return AYNI_INVALID_ARGUMENT;
    }
    out = {call, payload_size};
    size = call_fixed_size + call.function_name.size + call.protocol_id.size + payload_size;
    return AYNI_OK;
}

ayni_status_t plan_list(const ayni_tool_call_v1 *calls, uint32_t count, ListPlan &plan,
                        uint32_t &size, ayni_string_v1 assistant_text) {
    if (count == 0U || count > AYNI_CALL_TOOL_COUNT_MAX || calls == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto content_status =
        check_call_text(assistant_text, AYNI_CALL_RESULT_BYTES_MAX - list_header_size);
    if (content_status != AYNI_OK) {
        return content_status;
    }
    uint32_t total = list_header_size + assistant_text.size;
    for (uint32_t at = 0; at < count; ++at) {
        uint32_t call_size = 0;
        const auto status = read_call(calls[at], plan[at], call_size);
        if (status != AYNI_OK) {
            return status;
        }
        for (uint32_t before = 0; before < at; ++before) {
            if (catalog::compare_tokens(plan[before].source.protocol_id,
                                        plan[at].source.protocol_id) == 0) {
                return AYNI_INVALID_ARGUMENT;
            }
        }
        if (call_size > AYNI_CALL_RESULT_BYTES_MAX - total) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        total += call_size;
    }
    size = total;
    return AYNI_OK;
}

// Nested codecs write into the enclosing plan's reserved lane; no temporary
// payload-sized stack buffer or pointer into mutable Provider state is retained.
bool write_call(const CallValues &values, foundation::BodyWriter &writer) {
    const auto &source = values.source;
    writer.text(source.function_name);
    writer.u32(source.input_kind);
    writer.text(source.protocol_id);
    auto *body = writer.reserve_bytes(values.payload_size);
    if (body == nullptr) {
        return false;
    }
    uint32_t written = 0;
    if (source.input_kind == AYNI_TOOL_INPUT_INVALID_ARGUMENTS) {
        if (values.payload_size) {
            std::memcpy(body, source.input.invalid_arguments.data, values.payload_size);
        }
        return true;
    }
    const auto status =
        catalog::encode_arguments(source.input.arguments, body, values.payload_size, written);
    return status == AYNI_OK && written == values.payload_size;
}

bool read_encoded(ayni::foundation::Workspace &workspace, foundation::BodyReader &reader,
                  ToolCallView &out) {
    ToolCallView call{};
    call.function_name = reader.text(AYNI_CATALOG_ID_MAX_BYTES);
    call.input_kind = reader.u32();
    call.protocol_id = reader.text(AYNI_MODEL_TOOL_ID_BYTES_MAX);
    call.input = reader.bytes(AYNI_CALL_RESULT_BYTES_MAX);
    if (!reader.valid() || call.function_name.size == 0U || call.protocol_id.size == 0U ||
        check_call_text(call.function_name, AYNI_CATALOG_ID_MAX_BYTES) != AYNI_OK ||
        check_call_text(call.protocol_id, AYNI_MODEL_TOOL_ID_BYTES_MAX) != AYNI_OK) {
        return false;
    }
    if (call.input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
        auto value_lease = workspace.make<catalog::DecodedArguments>();
        if (!value_lease) {
            return false;
        }
        auto &value = *value_lease;
        if (catalog::decode_arguments(workspace, call.input, value) != AYNI_OK) {
            return false;
        }
    } else if (call.input_kind == AYNI_TOOL_INPUT_INVALID_ARGUMENTS) {
        if (check_call_text({reinterpret_cast<const char *>(call.input.data), call.input.size},
                            AYNI_CALL_RESULT_BYTES_MAX) != AYNI_OK) {
            return false;
        }
    } else {
        return false;
    }
    out = call;
    return true;
}

// Decode once for consumers that need the complete list. No output publishes
// until every source tree, protocol identity and exact end position was admitted.
bool decode(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body, ToolCallList &out) {
    if (body.data == nullptr || body.size < list_header_size ||
        body.size > AYNI_CALL_RESULT_BYTES_MAX) {
        return false;
    }
    foundation::BodyReader reader(body);
    if (reader.u32() != list_magic || reader.u32() != list_version) {
        return false;
    }
    const uint32_t count = reader.u32();
    if (count == 0U || count > AYNI_CALL_TOOL_COUNT_MAX) {
        return false;
    }
    ToolCallList candidate{};
    candidate.count = count;
    candidate.assistant_text = reader.text(AYNI_CALL_RESULT_BYTES_MAX);
    if (check_call_text(candidate.assistant_text, AYNI_CALL_RESULT_BYTES_MAX) != AYNI_OK) {
        return false;
    }
    for (uint32_t at = 0; at < count; ++at) {
        if (!read_encoded(workspace, reader, candidate.calls[at])) {
            return false;
        }
        for (uint32_t before = 0; before < at; ++before) {
            if (catalog::compare_tokens(candidate.calls[before].protocol_id,
                                        candidate.calls[at].protocol_id) == 0) {
                return false;
            }
        }
    }
    if (!reader.ok()) {
        return false;
    }
    out = candidate;
    return true;
}
} // namespace

ayni_status_t measure_tool_calls(const ayni_tool_call_v1 *calls, uint32_t count, uint32_t &size,
                                 ayni_string_v1 assistant_text) {
    ListPlan plan{};
    return plan_list(calls, count, plan, size, assistant_text);
}

// Re-admit at this separate public write boundary. Changed headers/identities
// cannot produce a partial valid list, including when they retain the old size.
void write_tool_calls(const ayni_tool_call_v1 *calls, uint32_t count, uint8_t *out, uint32_t size,
                      ayni_string_v1 assistant_text) {
    if (out == nullptr) {
        return;
    }
    ListPlan plan{};
    uint32_t required = 0;
    if (plan_list(calls, count, plan, required, assistant_text) != AYNI_OK || required != size) {
        std::memset(out, 0, size);
        return;
    }
    foundation::BodyWriter writer(out, size);
    writer.u32(list_magic);
    writer.u32(list_version);
    writer.u32(count);
    writer.text(assistant_text);
    for (uint32_t at = 0; at < count; ++at) {
        if (!write_call(plan[at], writer)) {
            std::memset(out, 0, size);
            return;
        }
    }
    if (!writer.complete()) {
        std::memset(out, 0, size);
    }
}

bool read_tool_calls(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body,
                     ToolCallList &out) {
    return decode(workspace, body, out);
}
uint32_t tool_call_count(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body) {
    ToolCallList decoded;
    return decode(workspace, body, decoded) ? decoded.count : 0U;
}
bool read_tool_call(ayni::foundation::Workspace &workspace, ayni_bytes_v1 body, uint32_t index,
                    ToolCallView &out) {
    ToolCallList decoded;
    if (!decode(workspace, body, decoded) || index >= decoded.count) {
        return false;
    }
    out = decoded.calls[index];
    return true;
}
} // namespace ayni::work

extern "C" ayni_status_t ayni_tool_calls_measure(const ayni_tool_call_v1 *calls, uint32_t count,
                                                 uint32_t *out_size) {
    return out_size == nullptr ? AYNI_INVALID_ARGUMENT
                               : ayni::work::measure_tool_calls(calls, count, *out_size);
}
