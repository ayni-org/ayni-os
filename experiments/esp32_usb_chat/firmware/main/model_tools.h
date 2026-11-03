#pragma once
#include <array>
#include <ayni/calls.h>
#include <cstddef>
namespace usbchat {
// Owns the bounded USB proposal bytes and node views until Core copies the result.
// Accepts the original single-tool payload and the batch-v1 envelope; no admission here.
class ModelTools {
  public:
    static constexpr size_t wire_bytes = 8192, nodes_per_tool = 32;
    ayni_status_t decode(ayni_bytes_v1 payload);
    const ayni_tool_call_v1 *calls() const { return calls_.data(); }
    uint32_t count() const { return count_; }

  private:
    bool decode_call(const uint8_t *data, size_t bytes, uint32_t index);
    std::array<uint8_t, wire_bytes> wire_{};
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> calls_{};
    std::array<std::array<ayni_argument_node_v1, nodes_per_tool>, AYNI_CALL_TOOL_COUNT_MAX>
        nodes_{};
    uint32_t count_ = 0; // Zero after failure; no partially decoded group is published.
};
} // namespace usbchat
