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
`destination_id`, the shared `model.slot` resource (default capacity 1), call capability, Provider and Binding. An authorized request
uses an OBJECT with a `body` TEXT and a `destination` IDENTIFIER default fixed by trusted configuration. Core derives the destination projection from that normalized object. The descriptor itself does
not grant permission. Core does not receive the API URL or credential.

A request body framed with `AYNI_MODEL_INSTRUCTIONS_PREFIX` (`<ayni/calls.h>`) is sent
as a system message holding the instructions and a user message holding the rest;
empty instructions send no system message, and any other body is one user message. A malformed frame fails with INVALID_ARGUMENT and
effect NONE before any HTTP request ([ADR-0074](../../docs/adr/0074-model-instructions-as-system-message.md)).

Functions are named `tool_0`, `tool_1`, etc. within each request; arguments use the original business Schema names and nested structure. Exact decimal and integer Schemas beyond the safe integer range use JSON strings; numeric lexemes never pass through double. Configure a read-only `tool_usage` port for every offered
tool. It supplies purpose, text grammar, result meaning, examples and explanations
matched by exact Binding/Capability versions and field IDs. All metadata is copied
in submit, capped at 4096 UTF-8 bytes per tool and 256 per field; missing/mismatched
or invalid metadata rejects before HTTP, without truncation or a Binding-ID fallback.
The port/context must outlive quiesce and must not block or call Core. The worker
never invokes it. The machine Schema retains original bounds/defaults and names; descriptions separately show one complete authorized candidate, including named projection paths, narrowed ranges/token sets and fixed route. Descriptions cannot expand authority. See
[the usage contract](../../contracts/host/tool_usage.md). Responses must contain
one finished choice, either final text or tools. Truncation and invalid arguments
fail with MODEL_OUTPUT; list encoding over the result byte limit also fails and
returns the completion borrow. A model proposal does not execute the tool.

All public methods and callbacks are owner-thread only. Call `ayni_begin_shutdown`,
drive the Kernel until it stops, call `quiesce()` until its outstanding post has returned
and its worker has joined, destroy the Kernel, then `close()` the Provider.
`close()` returns BUSY while a borrow remains and joins the worker on success.
Keep the object/context alive for as long as any Kernel has registered it. Poll
`status()` for an unexpected local transport/publication error; HTTP/model failures
are call outcomes. Cancellation reports CANCEL_UNKNOWN followed by ABANDONED;
a late cancellation cannot replace an already obtained result.

The default request/result text limit is 4096 bytes, response JSON limit 128 KiB,
concurrency 1, timeout 30 seconds, and `max_tokens` 1024. Configure smaller bounds
as needed; an unsupported server parameter is an error, never silently removed.
JSON depth is 8, array length is 64 (objects follow the 128-node Schema bound), and a tool result holds at most 8 calls.
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

P2 adds explicit `result_format=AYNI_CALL_RESULT_TEXT` (`openai.text`) for summaries.
The default remains TOOL_CALLS (`openai.chat`). Text calls receive no tool view.
