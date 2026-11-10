# Native OpenAI-compatible Provider

The public C++17 `Provider` uses one curl multi worker and the optional
[`Ayni::ModelProtocol`](../model_protocol/README.md) SDK component. The worker
never queries Core. Configure the API root (optionally `/v1`), model, key environment
variable name, body store and declared Provider/endpoint/destination IDs. HTTPS is
required except for numeric loopback fixtures. TLS verification stays enabled;
redirects, proxies, netrc, retries and service fallback are disabled.

Defaults remain non-streaming Chat Completions, `max_tokens`, TEXT reply encoding
and four discovery functions. `Config::api`, `stream`, `chat_token_limit`,
`reply_encoding`, `extra_parameters` and `preview` explicitly select new behavior.
Responses uses `/responses`, native Items/tools/call_id, `store:false` and encrypted
reasoning continuation. It requires FRAMED replies and a matching ToolLoop decoder
and protocol projection. Native binds continuation to `base_url` and model. To
measure requests in the SDK, configure RequestOptions with the same selection and
use `Protocol::projection`; its borrowed options must outlive the ToolLoop.

The composition registers the public MODEL capability, destination authority,
resource, Provider and Binding. Core still performs egress, authorization and Task
limits. Main TOOL_CALLS and summary TEXT contracts remain distinct. Instructions
and history use the public model frames; optional request-only tools replace the
fixed discovery set. Schemas and arguments are never guessed or repaired. Original
numeric lexemes survive typed argument decoding. Serving-model usage is distinct
from the estimated preflight count and malformed optional statistics do not erase
a completed answer.

`submit` copies an inline body or reference. The worker reads/verifies requests,
handles HTTP/SSE and writes the selected full reply to the body store before
publishing its reference. Output buffers grow on demand with a defensive bound
of 64 KiB metadata plus 96 bytes per configured output token. Input arguments keep
depth 8/array length 64 limits; protocol envelopes allow echoed schemas to depth 32
and the registered directory bound. At most eight calls occur in one answer.
Preview text is tentative and worker-only; it cannot execute tools or reenter Core.

After Kernel STOPPED, call `quiesce()` until posts return and the worker joins;
destroy the Kernel, then `close()` the Provider. BUSY retains the context/borrows.
Cancellation reports CANCEL_UNKNOWN followed by ABANDONED; a late cancellation
cannot replace a known terminal. Terminal working copies are erased after transfer.
HTTP/model outcomes stay per-call; `status()` reports unexpected local transport or
publication failures. No images, remote outcome query or automatic retry is offered.

Normal validation uses only local HTTP/SSE fixtures. The manual live probe remains
excluded from default builds and requires separate explicit authorization. Current
local checks do not establish OpenAI/DeepSeek online compatibility or hardware
acceptance. See [ADR-0098](../../docs/adr/0098-shared-model-protocol-and-preloaded-tools.md).
