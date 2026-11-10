#pragma once
#include <array>
#include <ayni/calls.h>
#include <ayni/workspace.h>
namespace wifichat {
// Owns a public SDK tool-list body and bounded node views until Core copies it.
class ModelTools {
  public:
    static constexpr size_t wire_bytes = 8192, nodes_per_tool = 32;
    ayni_status_t decode(ayni_bytes_v1, const ayni_workspace_v1 &);
    const ayni_tool_call_v1 *calls() const { return calls_.data(); }
    uint32_t count() const { return count_; }
    // JSON progress metadata: function, binding and fixed operation enum. No business
    // argument value or assistant text crosses this UI lane; no execution approval.
    ayni_status_t outline(char *out, size_t capacity, size_t &written) const;
    ayni_string_v1 assistant() const { return assistant_; }

  private:
    std::array<uint8_t, wire_bytes> wire_{};
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> calls_{};
    std::array<std::array<ayni_argument_node_v1, nodes_per_tool>, AYNI_CALL_TOOL_COUNT_MAX>
        nodes_{};
    uint32_t count_ = 0;
    ayni_string_v1 assistant_{};
};
} // namespace wifichat
