#ifndef AYNI_PROVIDERS_MODEL_PROTOCOL_H
#define AYNI_PROVIDERS_MODEL_PROTOCOL_H
#include <array>
#include <ayni/calls.h>
#include <ayni/model_messages.hpp>
#include <ayni/values_json.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace ayni::providers::model_protocol {
// Explicit wire choices; model names never select a protocol or budget field.
enum class Api { chat_completions, responses };
enum class ChatTokenLimit { max_tokens, max_completion_tokens };
std::string_view path(Api api);
// No credentials or HTTP backend belong here. Views borrow immutable composition-owned
// storage through each synchronous encode/decode, through Stream destruction, and through all
// uses of a Projection. A Projection also borrows this options object itself.
struct RequestOptions {
    std::string_view model;
    uint32_t max_output_tokens = 1024;
    uint32_t result_format = AYNI_CALL_RESULT_TOOL_CALLS;
    // Optional trusted JSON object; cannot override protocol-owned fields. Validated before
    // writing. Used for explicit service extensions, never guessed or silently removed.
    std::string_view extra_parameters;
    Api api = Api::chat_completions;
    bool stream = false;
    ChatTokenLimit chat_token_limit = ChatTokenLimit::max_tokens;
    model::ReplyEncoding reply_encoding = model::ReplyEncoding::text;
    // Exact immutable egress route, no credentials. Responses continuation binds route+model;
    // changing either requires an explicit new conversation, never silent state dropping.
    std::string_view continuation_scope;
};
inline constexpr uint32_t json_depth_max = 8;
inline constexpr uint32_t json_container_max = 64;
inline constexpr uint32_t envelope_depth_max = 32;     // Echoed function schemas and SSE envelope.
inline constexpr uint32_t control_schema_branches = 2; // Ref controls and wait.
inline constexpr uint32_t envelope_array_max =
    AYNI_GOAL_TYPE_TOOL_COUNT_MAX + control_schema_branches;
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
    ~Answer();
    void clear(); // Wipe private storage after final publication or before reuse.
    ayni_provider_result_v1 result{};
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX> calls{};
    std::array<std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX>,
               AYNI_CALL_TOOL_COUNT_MAX>
        arguments{};
    Buffer body;
    size_t used = 0;
    // Internal reply bytes, separate from canonical text/arguments. Views remain valid until
    // the next decode. encode_answer embeds this state only for explicitly framed consumers.
    std::string continuation;
    std::string reply;

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
    void decode(const RequestOptions &, char *buffer, size_t size, Answer &answer) const;
    // Same fields used by write_request and SDK budget projection; no model/HTTP options.
    ayni_status_t write_context(json::Writer &, const RequestOptions &, ayni_string_v1, model::Mode,
                                model::Completeness = model::Completeness::complete) const;
    static model::Projection projection(RequestOptions &options);
};
// Encodes a completed answer for the body store or a bounded device queue. Tool lists keep
// the existing public binary codec; their assistant lane carries the selected reply frame.
ayni_status_t encode_answer(const RequestOptions &, Answer &, std::string &out);
// Preview is tentative text, never authority. Called on the I/O worker, borrowed until return;
// it must not throw, reenter Core, execute tools or block. No callback implies no preview.
struct Preview {
    void *context = nullptr;
    void (*text)(void *, std::string_view) = nullptr;
};
// One request's bounded SSE accumulator. feed accepts arbitrary byte cuts/CRLF/data lines.
// finish is called only after HTTP succeeds; only then can a complete Answer be published.
// Destruction wipes buffered model data. No retry, thread, credential or I/O belongs here.
class Stream {
  public:
    Stream(const RequestOptions &, size_t byte_limit, Preview = {});
    ~Stream();
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
    ayni_status_t status() const; // Initial allocation or sticky feed status; no I/O.
    ayni_status_t feed(std::string_view bytes);
    void finish(Answer &answer);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::providers::model_protocol
#endif
