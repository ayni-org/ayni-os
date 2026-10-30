#ifndef AYNI_OPENAI_COMPAT_PROTOCOL_H
#define AYNI_OPENAI_COMPAT_PROTOCOL_H
#include <array>
#include <ayni/detail/tool_usage.hpp>
#include <ayni/providers/openai_compat.h>
#include <cstddef>
#include <memory>

namespace ayni::providers::openai_compat {
constexpr uint32_t json_depth_max = 8;
constexpr uint32_t json_container_max = 64;
constexpr size_t request_json_max = 1048576;
// Owns a submit-time view and every string it references. Never move after copying.
struct Tool {
    Tool() = default;
    Tool(const Tool &) = delete;
    Tool &operator=(const Tool &) = delete;
    Tool(Tool &&) = delete;
    Tool &operator=(Tool &&) = delete;
    ayni_tool_v1 view{};
    static constexpr size_t string_count =
        2 + AYNI_AUTHORITY_FIELD_COUNT_MAX * (1 + AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX);
    std::array<char, string_count * AYNI_CATALOG_ID_MAX_BYTES> strings{};
    std::array<uint8_t, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX> original_domain{};
    std::array<uint8_t, AYNI_BUSINESS_CONTRACT_BYTES_MAX> business_contract{};
    ayni_tool_usage_v1 usage{}; // Read-only view into usage_storage; never a caller-owned string.
    std::array<ayni_tool_field_usage_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> field_usage{};
    detail::ToolUsage usage_storage;
    bool copy(const ayni_tool_v1 &source); // Owner-valid Core view; copies every borrowed string.
    // Exact pairing, fields, UTF-8 and byte budgets; no allocation or fallback.
    bool copy_usage(const ayni_tool_usage_v1 &source);
};
// Result views borrow only tools and this answer's fixed body storage, never a JSON DOM.
struct Answer {
    ayni_provider_result_v1 result{};
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> calls{};
    std::array<std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX>,
               AYNI_CALL_TOOL_COUNT_MAX>
        arguments{};
    std::unique_ptr<char[]> body;
    size_t used = 0;
    void fail(ayni_status_t status, ayni_reason_t reason);
};
// Worker-owned protocol scratch. Fixed pools bound yyjson's allocations, including
// the second parse for function.arguments. No JSON or raw error crosses the ABI.
class Protocol {
  public:
    explicit Protocol(size_t response_limit);
    // INVALID_ARGUMENT for a malformed instructions frame, CAPACITY_EXCEEDED past the JSON limit.
    ayni_status_t encode(const Config &config, ayni_string_v1 input, const Tool *tools,
                         uint32_t tool_count, char *buffer, size_t &size) const;
    void decode(char *buffer, size_t size, const Tool *tools, uint32_t tool_count,
                uint32_t result_limit, Answer &answer);

  private:
    size_t pool_size_;
    std::unique_ptr<char[]> outer_pool_;
    std::unique_ptr<char[]> arguments_pool_;
};
} // namespace ayni::providers::openai_compat
#endif
