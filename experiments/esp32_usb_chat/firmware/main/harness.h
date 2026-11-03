// The seven run settings of this deployment (ADR-0095), in one place. They size the ToolLoop's
// context, the Core turn limit, the model contracts and the request the Mac bridge sends.
#pragma once
#include <cstdint>
namespace usbchat::harness {
// A request holds at most this many estimated tokens, max_output_tokens of them left for the
// answer. Chinese text estimates about 4 UTF-8 bytes per token, so a full window still fits the
// 32 KiB USB request lane with the four function definitions beside it.
inline constexpr uint64_t context_window_tokens = 8192;
inline constexpr uint64_t max_output_tokens = 1800;
inline constexpr uint64_t compact_target_tokens = 4096;
inline constexpr uint64_t tool_result_max_tokens = 1024;
// Model calls a turn makes before Core pauses it. The sequential speaker check alone takes 8
// (seven single-tool rounds and the answer); the rest leaves room for a history summary.
inline constexpr uint32_t max_turns = 12;
inline constexpr uint32_t request_timeout_ms = 90000;
inline constexpr uint32_t model_concurrency = 1; // The bridge serves one model request at a time.
} // namespace usbchat::harness
