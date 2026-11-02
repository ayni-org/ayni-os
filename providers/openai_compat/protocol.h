#ifndef AYNI_OPENAI_COMPAT_PROTOCOL_H
#define AYNI_OPENAI_COMPAT_PROTOCOL_H
#include <array>
#include <ayni/providers/openai_compat.h>
#include <cstddef>
#include <string>

namespace ayni::providers::openai_compat {
constexpr uint32_t json_depth_max = 8;
constexpr uint32_t json_container_max = 64;
// Result views borrow this answer's body storage, never a JSON DOM. decode sizes it once from
// the response, so the views never move (ADR-0095).
struct Answer {
    ayni_provider_result_v1 result{};
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> calls{};
    std::array<std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX>,
               AYNI_CALL_TOOL_COUNT_MAX>
        arguments{};
    std::string body;
    size_t used = 0;
    void fail(ayni_status_t status, ayni_reason_t reason);
};
// Worker-owned protocol. Each decode sizes yyjson's pools from the response it parses,
// including the second parse for function.arguments. No JSON or raw error crosses the ABI.
class Protocol {
  public:
    // Measures the request first and allocates it once. INVALID_ARGUMENT for a malformed
    // instructions frame.
    ayni_status_t encode(const Config &config, ayni_string_v1 input, std::string &out) const;
    void decode(char *buffer, size_t size, Answer &answer) const;
};
} // namespace ayni::providers::openai_compat
#endif
