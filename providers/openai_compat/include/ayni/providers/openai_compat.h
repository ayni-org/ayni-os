// Native non-streaming Chat Completions adapter (ADR-0055). No Core-private dependency.
#ifndef AYNI_PROVIDERS_OPENAI_COMPAT_H
#define AYNI_PROVIDERS_OPENAI_COMPAT_H
#include <ayni/bodies.h>
#include <ayni/calls.h>
#include <memory>
#include <string>

namespace ayni::providers::openai_compat {
// The contract's inline bounds (ADR-0096). A request value holds its destination and either a
// short inline body or a body reference; every answer is written to the body store, so no
// result text is kept inline. A composition root sizes Core's call bytes from these.
inline constexpr uint32_t call_input_bytes_max = 16384;
inline constexpr uint32_t call_result_bytes_max = 0;
// Trusted startup configuration, copied by configure. No secret value belongs here.
struct Config {
    std::string base_url; // HTTPS API root, optionally with /v1; appends /chat/completions.
    std::string model;
    std::string key_environment; // Name only; Provider reads its value once at configure.
    std::string provider_id = "openai.compat";
    std::string endpoint_id = "openai.chat";
    std::string destination_id = "openai.remote";
    uint32_t request_capacity = 1; // 1..8 concurrent requests; fixed at configure.
    uint32_t timeout_ms = 30000;   // From submit, including the local queue; 1..300000 ms.
    // Sent as max_tokens, positive. Buffers grow with each request and its response; the
    // response is refused past what this many output tokens can need (ADR-0095).
    uint32_t max_output_tokens = 1024;
    // TEXT declares openai.text and offers no tools (conversation summaries);
    // TOOL_CALLS preserves openai.chat. Other formats are rejected.
    uint32_t result_format = AYNI_CALL_RESULT_TOOL_CALLS;
    // Required. Request bodies are read and verified here, and each answer (text, or the
    // tool-call list encoding) is written here before it is posted. Outlives close.
    const ayni_body_store_v1 *bodies = nullptr;
};
// Owner-thread configure/describe/close and callbacks; one internal I/O worker.
// submit copies an inline body or a body reference without I/O or waiting. The worker reads
// a referenced body, refusing one whose length or digest differs, and writes each answer to
// the body store, posting its reference once durable. Worker calls only these and provider_post.
// TLS is verified; redirects, proxies, retries and fallback are disabled. Plain HTTP is
// accepted only for numeric loopback protocol fixtures (127.0.0.1 or [::1]).
class Provider {
  public:
    Provider();
    ~Provider(); // Call close explicitly; lifetime must outlast registered Kernels.
    Provider(const Provider &) = delete;
    Provider &operator=(const Provider &) = delete;
    ayni_status_t configure(const Config &config);
    // Nested descriptor views remain valid until close; registration copies them.
    ayni_status_t describe(ayni_provider_v1 &out) const;
    // After the registered Kernel is STOPPED, join the worker while keeping its
    // registered context/descriptor alive until Kernel destruction. BUSY while
    // a post is still returning; repeat after yielding. No further submits allowed.
    ayni_status_t quiesce();
    // BUSY while any borrow remains. After Kernel drain, joins the worker. Keep the
    // configured context until Kernel destruction; call close after destroy succeeds.
    ayni_status_t close();
    uint32_t pending() const;     // Accepted borrows, including publication awaiting retry.
    ayni_status_t status() const; // First unexpected transport/publication failure, or OK.

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::providers::openai_compat
#endif
