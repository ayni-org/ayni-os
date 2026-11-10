#ifndef AYNI_PROVIDERS_MODEL_PROTOCOL_H
#define AYNI_PROVIDERS_MODEL_PROTOCOL_H
#include <array>
#include <ayni/calls.h>
#include <ayni/values_json.hpp>
#include <cstddef>
#include <string>
#include <string_view>

namespace ayni::providers::model_protocol {
// Borrowed for one synchronous encode only. No credentials or HTTP backend belong here.
struct RequestOptions {
    std::string_view model;
    uint32_t max_output_tokens = 1024;
    uint32_t result_format = AYNI_CALL_RESULT_TOOL_CALLS;
    // Optional trusted JSON object; cannot override protocol-owned fields. Validated before
    // writing. Used for explicit service extensions, never guessed or silently removed.
    std::string_view extra_parameters;
};
inline constexpr uint32_t json_depth_max = 8;
inline constexpr uint32_t json_container_max = 64;
// Explicitly allocated, stable private storage. resize failure preserves the old buffer;
// reset/destruction erase it. One worker owns it; no borrowed view survives resizing.
class Buffer {
  public:
    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    bool resize(size_t size);
    void reset();
    char *data() { return data_; }
    const char *data() const { return data_; }
    size_t size() const { return size_; }

  private:
    char *data_ = nullptr;
    size_t size_ = 0;
};
// Result views borrow this answer's body storage, never a JSON DOM. decode sizes it once from
// the response, so the views never move (ADR-0095).
struct Answer {
    ayni_provider_result_v1 result{};
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> calls{};
    std::array<std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX>,
               AYNI_CALL_TOOL_COUNT_MAX>
        arguments{};
    Buffer body;
    size_t used = 0;
    void fail(ayni_status_t status, ayni_reason_t reason);
};
// Worker-owned protocol. Each decode sizes yyjson's pools from the response it parses,
// including the second parse for function.arguments. No JSON or raw error crosses the ABI.
class Protocol {
  public:
    // Measures the request first and allocates it once. INVALID_ARGUMENT for a malformed
    // instructions frame.
    ayni_status_t encode(const RequestOptions &config, ayni_string_v1 input,
                         std::string &out) const;
    // Same writer for bounded platform buffers and Native allocation/measurement.
    ayni_status_t write_request(ayni::json::Writer &, const RequestOptions &,
                                ayni_string_v1 input) const;
    void decode(char *buffer, size_t size, Answer &answer) const;
};
} // namespace ayni::providers::model_protocol
#endif
