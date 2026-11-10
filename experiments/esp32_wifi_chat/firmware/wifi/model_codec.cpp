#include "model_codec.h"
#include "../main/model_tools.h"
#include <ayni/providers/model_protocol.h>
#include <cstring>
#include <memory>
#include <new>
namespace wifichat::wifi {
bool decode_model(std::string_view response, bool tools_allowed, Packet &packet) {
    constexpr size_t response_bytes_max = 32768;
    if (response.empty() || response.size() > response_bytes_max) {
        return false;
    }
    // The shared codec owns all JSON/argument parsing. This adapter only copies a complete
    // answer to the experiment queue, enforcing that queue's smaller device limits.
    auto answer = std::unique_ptr<ayni::providers::model_protocol::Answer>(
        new (std::nothrow) ayni::providers::model_protocol::Answer{});
    if (!answer) {
        return false;
    }
    const ayni::providers::model_protocol::Protocol codec;
    codec.decode(const_cast<char *>(response.data()), response.size(), *answer);
    const auto &result = answer->result;
    if (result.outcome != AYNI_CALL_COMPLETED || result.status != AYNI_OK) {
        return false;
    }
    if (!result.tool_call_count) {
        if (!result.result.size || result.result.size > summary_bytes) {
            return false;
        }
        packet.kind = Kind::model_text;
        packet.size = result.result.size;
        std::memcpy(packet.body.data(), result.result.data, result.result.size);
        return true;
    }
    if (!tools_allowed || result.result.size > answer_bytes) {
        return false;
    }
    for (uint32_t at = 0; at < result.tool_call_count; ++at) {
        const auto &call = result.tool_calls[at];
        if (call.input_kind == AYNI_TOOL_INPUT_ARGUMENTS &&
            call.input.arguments.node_count > ModelTools::nodes_per_tool) {
            return false;
        }
    }
    uint32_t written = 0;
    if (ayni_tool_calls_write(result.tool_calls, result.tool_call_count, result.result,
                              packet.body.data(), ModelTools::wire_bytes, &written) != AYNI_OK) {
        return false;
    }
    packet.kind = Kind::model_tools;
    packet.size = written;
    return true;
}
} // namespace wifichat::wifi
