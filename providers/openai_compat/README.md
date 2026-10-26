# Native Chat Completions Provider

`ayni::providers::openai_compat::Provider` implements non-streaming text and typed
function proposals through the public C ABI. It has one libcurl multi worker;
only `submit` reads the tool-view callback. See [ADR-0055](../../docs/adr/0055-openai-compatible-provider.md).

Configure `base_url`, `model`, `key_environment` (the environment variable name,
never its value), and trusted Provider/endpoint/destination IDs before registering.
The API root may include `/v1`; the adapter appends `/chat/completions`. HTTPS is
required except for numeric loopback fixtures. TLS verification stays enabled;
redirects, ambient proxies, netrc, automatic retries and service fallback are disabled.

`describe` provides the `openai.chat` call contract. The composition root registers
an authority descriptor with one IDENTIFIER egress-destination field containing
`destination_id`, the call capability, Provider and Binding. An authorized request
must contain that destination as its concrete argument. The descriptor itself does
not grant permission. Core does not receive the API URL or credential.

Functions are named `tool_0`, `tool_1`, etc. within each request. Their description is
the offered Binding ID; arguments are `field_<id>` and `text`. Integer units/ranges
and token enums come from that request's authorized view. Responses must contain
one finished choice, either final text or tools. Truncation and invalid arguments
fail with MODEL_OUTPUT; list encoding over the result byte limit also fails and
returns the completion borrow. A model proposal does not execute the tool.

All public methods and callbacks are owner-thread only. Call `ayni_begin_shutdown`,
drive the Kernel until it stops, destroy it successfully, then `close()` the Provider.
`close()` returns BUSY while a borrow remains and joins the worker on success.
Keep the object/context alive for as long as any Kernel has registered it. Poll
`status()` for an unexpected local transport/publication error; HTTP/model failures
are call outcomes. Cancellation reports CANCEL_UNKNOWN followed by ABANDONED;
a late cancellation cannot replace an already obtained result.

The default request/result text limit is 4096 bytes, response JSON limit 128 KiB,
concurrency 2, timeout 30 seconds, and `max_tokens` 1024. Configure smaller bounds
as needed; an unsupported server parameter is an error, never silently removed.
JSON depth is 8, container length is 64, and a tool result holds at most 8 calls.
The Provider owns fixed protocol buffers; libcurl/TLS retain their own bounded-by-
concurrency allocations. No streaming, images, remote outcome query, or conversation
store is implemented here.

The normal tests use only a loopback server. For the explicitly authorized live probe:

```sh
cmake --build build/native --target ayni_openai_smoke
# Supply AYNI_LLM_API_KEY in the process environment without recording it in shell history.
AYNI_LLM_BASE_URL=https://api.deepseek.com AYNI_LLM_MODEL=deepseek-chat \
  build/native/tests/unit/ayni_openai_smoke --live
```

This sends two synthetic prompts and caps each output at 128 tokens. It is excluded
from default builds and CTest. It prints assertions, not request headers or raw vendor
responses. DeepSeek has been checked for these two operations; other services and
long-running use require separate evidence.
