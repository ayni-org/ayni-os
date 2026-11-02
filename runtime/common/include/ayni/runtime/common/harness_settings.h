// The harness's seven deployment settings (ADR-0095), passed by a composition root to the
// ToolLoop, the model Providers and Core. Nothing else bounds a message, answer, summary or tool
// result; every working copy is allocated on demand. Header-only and free of strategy types.
#ifndef AYNI_RUNTIME_COMMON_HARNESS_SETTINGS_H
#define AYNI_RUNTIME_COMMON_HARNESS_SETTINGS_H
#include <ayni/ayni.h>
#include <cstdint>

namespace ayni::runtime {
struct HarnessSettings {
    // Tokens one model request may hold, the deployment's actual model window.
    uint64_t context_window_tokens = 32768;
    // Tokens left for the answer; also sent as the request's output limit.
    uint64_t max_output_tokens = 1024;
    // Size compaction shrinks the context to; also bounds the user inputs kept after a reset.
    uint64_t compact_target_tokens = 8192;
    // Largest tool result sent as is; a larger one is archived and summarized.
    uint64_t tool_result_max_tokens = 4096;
    // Model calls one turn may make before it pauses; the owner may raise it to the ceiling.
    uint32_t max_turns = AYNI_MAX_TURNS_DEFAULT;
    // Bound of one model request, including its wait for a free slot.
    uint32_t request_timeout_ms = 30000;
    // Model requests in flight at once, across every model binding.
    uint32_t model_concurrency = 1;
};
// The relations the ToolLoop needs: room for the answer, one tool result and the target.
inline bool valid(const HarnessSettings &s) {
    const auto window = s.context_window_tokens, output = s.max_output_tokens;
    if (!window || !output || output >= window || !s.tool_result_max_tokens ||
        s.tool_result_max_tokens >= window - output || !s.compact_target_tokens ||
        s.compact_target_tokens >= window - output - s.tool_result_max_tokens) {
        return false;
    }
    return s.max_turns && s.max_turns <= AYNI_MAX_TURNS_MAX && s.request_timeout_ms &&
           s.model_concurrency;
}
} // namespace ayni::runtime
#endif
