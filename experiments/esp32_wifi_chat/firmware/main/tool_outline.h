#pragma once
#include "protocol.h"
namespace wifichat {
// Worker-local projection of an already decoded Answer. Only protocol identities escape;
// no complete wire, node array, assistant text or business arguments are retained for UI.
ayni_status_t write_tool_outline(const ayni_provider_result_v1 &, char *, size_t, size_t &);
struct ToolOutline {
    std::array<char, tool_outline_bytes> bytes{};
    uint32_t size = 0; // Zero when absent or optional rendering failed.
    void clear() {
        bytes.fill(0);
        size = 0;
    }
    void capture(const ayni_provider_result_v1 &result) {
        clear();
        size_t written = 0;
        if (result.tool_call_count &&
            write_tool_outline(result, bytes.data(), bytes.size(), written) == AYNI_OK) {
            size = static_cast<uint32_t>(written);
        }
    }
};
} // namespace wifichat
