#include "model_codec.h"
#include "../main/model_tools.h"
#include <algorithm>
#include <ayni/providers/model_protocol.h>
#include <cstring>
#include <memory>
#include <new>
namespace wifichat::wifi {
void model_failure(Packet &packet, ayni_status_t status, ayni_reason_t reason, uint32_t effect) {
    ayni_provider_result_v1 result{};
    result.outcome = status == AYNI_RESULT_UNKNOWN ? AYNI_CALL_ABANDONED : AYNI_CALL_FAILED;
    result.status = status;
    result.reason = status == AYNI_RESULT_UNKNOWN ? AYNI_REASON_NONE : reason;
    result.effect = status == AYNI_RESULT_UNKNOWN ? AYNI_CALL_EFFECT_UNKNOWN : effect;
    write_model_result_header(packet.body.data(), result);
    packet.kind = Kind::model_error;
    packet.size = model_result_header_bytes;
}
bool package_model(const ayni::providers::model_protocol::RequestOptions &options,
                   ayni::providers::model_protocol::Answer &answer, bool tools_allowed,
                   Packet &packet) {
    const auto &result = answer.result;
    if (result.outcome != AYNI_CALL_COMPLETED || result.status != AYNI_OK ||
        (!tools_allowed && result.tool_call_count)) {
        if (result.status != AYNI_OK && result.outcome != AYNI_CALL_COMPLETED) {
            write_model_result_header(packet.body.data(), result);
            packet.kind = Kind::model_error;
            packet.size = model_result_header_bytes;
        } else {
            model_failure(packet, AYNI_INVALID_ARGUMENT, AYNI_REASON_MODEL_OUTPUT);
        }
        return false;
    }
    for (uint32_t at = 0; at < result.tool_call_count; ++at) {
        const auto &call = result.tool_calls[at];
        if (call.input_kind == AYNI_TOOL_INPUT_ARGUMENTS &&
            call.input.arguments.node_count > ModelTools::nodes_per_tool) {
            model_failure(packet, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
            return false;
        }
    }
    std::string encoded;
    const auto encoded_status =
        ayni::providers::model_protocol::encode_answer(options, answer, encoded);
    if (encoded_status != AYNI_OK || encoded.empty() ||
        encoded.size() > packet.body.size() - model_result_header_bytes) {
        model_failure(packet, encoded_status != AYNI_OK ? encoded_status : AYNI_CAPACITY_EXCEEDED,
                      AYNI_REASON_MODEL_OUTPUT);
        std::fill(encoded.begin(), encoded.end(), 0);
        return false;
    }
    packet.kind = Kind::model_result;
    write_model_result_header(packet.body.data(), result);
    std::memcpy(packet.body.data() + model_result_header_bytes, encoded.data(), encoded.size());
    packet.size = static_cast<uint32_t>(model_result_header_bytes + encoded.size());
    std::fill(encoded.begin(), encoded.end(), 0);
    return true;
}
bool decode_model(std::string_view response, bool tools_allowed, Packet &packet,
                  const ayni::providers::model_protocol::RequestOptions &options) {
    constexpr size_t response_bytes_max = 32768;
    if (response.empty() || response.size() > response_bytes_max) {
        model_failure(packet,
                      response.size() > response_bytes_max ? AYNI_CAPACITY_EXCEEDED
                                                           : AYNI_INVALID_ARGUMENT,
                      AYNI_REASON_MODEL_OUTPUT);
        return false;
    }
    auto answer = std::unique_ptr<ayni::providers::model_protocol::Answer>(
        new (std::nothrow) ayni::providers::model_protocol::Answer{});
    if (!answer) {
        model_failure(packet, AYNI_CAPACITY_EXCEEDED, AYNI_REASON_MODEL_OUTPUT);
        return false;
    }
    ayni::providers::model_protocol::Protocol{}.decode(options, const_cast<char *>(response.data()),
                                                       response.size(), *answer);
    return package_model(options, *answer, tools_allowed, packet);
}
} // namespace wifichat::wifi
