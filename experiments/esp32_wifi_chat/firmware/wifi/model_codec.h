#pragma once
#include "../main/protocol.h"
#include <string_view>
namespace wifichat::wifi {
// Accept one complete assistant response. Only explicit action turns admit tool lists;
// ordinary replies and summaries must contain final text.
bool decode_model(std::string_view response, bool tools_allowed, Packet &result);
} // namespace wifichat::wifi
