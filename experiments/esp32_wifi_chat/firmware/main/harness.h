// The seven run settings of this deployment (ADR-0095), in one place. They size the ToolLoop's
// context, the Core turn limit, the model contracts and the request the Wi-Fi worker sends.
#pragma once
#include <cstdint>
namespace wifichat::harness {
// ToolLoop's total estimated token budget, including reserved model output.
// BodyRef transport keeps large prompts out of ordinary fixed message queues.
inline constexpr uint64_t context_window_tokens = 128 * 1024;
inline constexpr uint32_t model_body_bytes = 768 * 1024;
inline constexpr uint64_t max_output_tokens = 1800;
inline constexpr uint64_t compact_target_tokens = 4096;
inline constexpr uint64_t tool_result_max_tokens = 1024;
// Model turns before Core pauses a Task; room for discovery, inspection,
// managed-operation waits, a final answer and context compaction.
inline constexpr uint32_t max_turns = 12;
inline constexpr uint32_t request_timeout_ms = 90000;
inline constexpr uint32_t model_concurrency = 1; // One worker serves model requests.
} // namespace wifichat::harness
