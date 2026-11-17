#pragma once
#include "../main/tool_outline.h"
#include <string_view>
namespace wifichat::wifi {
// Accept one complete assistant response. Only explicit action turns admit tool lists;
// ordinary replies and summaries must contain final text.
// Classified complete HTTP/protocol refusal; no raw vendor data enters this packet.
void model_failure(Packet &, ayni_status_t, ayni_reason_t = AYNI_REASON_NONE,
                   uint32_t effect = AYNI_CALL_EFFECT_UNKNOWN);
bool package_model(const ayni::providers::model_protocol::RequestOptions &,
                   ayni::providers::model_protocol::Answer &, bool tools_allowed, Packet &result,
                   ToolOutline *outline = nullptr);
bool decode_model(std::string_view response, bool tools_allowed, Packet &result,
                  const ayni::providers::model_protocol::RequestOptions &options = {},
                  ToolOutline *outline = nullptr);
} // namespace wifichat::wifi
