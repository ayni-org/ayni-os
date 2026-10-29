// Native non-streaming Chat Completions adapter (ADR-0055). No Core-private dependency.
#ifndef AYNI_PROVIDERS_OPENAI_COMPAT_H
#define AYNI_PROVIDERS_OPENAI_COMPAT_H
#include <ayni/tool_usage.h>
#include <memory>
#include <string>

namespace ayni::providers::openai_compat {
// Trusted startup configuration, copied by configure. No secret value belongs here.
struct Config {
    std::string base_url; // HTTPS API root, optionally with /v1; appends /chat/completions.
    std::string model;
    std::string key_environment; // Name only; Provider reads its value once at configure.
    std::string provider_id = "openai.compat";
    std::string endpoint_id = "openai.chat";
    std::string destination_id = "openai.remote";
    uint32_t request_capacity = 1;        // 1..8 concurrent requests; fixed at configure.
    uint32_t text_max_bytes = 4096;       // Input and result limit, 1..CALL_TEXT_BYTES_MAX.
    uint32_t response_max_bytes = 131072; // Whole HTTP body, 1..1048576 bytes.
    uint32_t timeout_ms = 30000;          // From submit, including the local queue; 1..300000 ms.
    uint32_t max_output_tokens = 1024;    // Sent as max_tokens, 1..8192; distinct from byte limits.
    // TEXT declares openai.text and offers no tools (conversation summaries);
    // TOOL_CALLS preserves openai.chat. Other formats are rejected.
    uint32_t result_format = AYNI_CALL_RESULT_TOOL_CALLS;
    // Required for each offered tool. Copied port; runtime owns context through quiesce.
    // No tools (e.g. TEXT summaries) need no port. Worker never invokes it.
    ayni_tool_usage_port_v1 tool_usage{};
};
// Owner-thread configure/describe/close and callbacks; one internal I/O worker.
// submit copies views without I/O, allocation or waiting. Worker calls only provider_post.
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
