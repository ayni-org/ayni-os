# Shared model protocol

`Ayni::ModelProtocol` is an optional C++17 source SDK component. It owns Chat
Completions and Responses encoding, bounded JSON/argument decoding, usage and SSE.
It imports only public Core codecs. HTTP, credentials, threads, body stores and
execution remain with the consuming transport/composition.

Use `RequestOptions` to select `api`, `stream`, the Chat output-limit field and
`reply_encoding`. Responses requires `ReplyEncoding::framed` and a non-secret,
immutable `continuation_scope` identifying the exact egress route. Configure the
ToolLoop with matching main/summary reply encodings. `encode_answer` writes the
selected reply or public tool-list body; store it before publishing its BodyRef.
`Answer` owns copied views until clear/destruction; private storage is erased.

For a ToolLoop using this protocol, keep options alive and immutable and set:

```cpp
config.projection = ayni::providers::model_protocol::Protocol::projection(options);
config.reply_encoding = config.summary_reply_encoding = options.reply_encoding;
config.tool_exposure = ayni::strategies::ToolExposure::preloaded; // optional
```

The projection owns no state. Its callbacks use the same context/tool writers as
transport, including continuation and request-only definitions. Preloaded mode
requires a projection. Serving-model usage is reported separately from preflight
estimation; projected requests do not reuse legacy Chat character anchors.

Feed each successful SSE response into a fresh `Stream`; arbitrary cuts, CRLF and
multiple data lines are supported. Check initial `status()` before HTTP. Invoke
`finish` only after successful complete HTTP EOF. Only a completed Answer may be
published. Preview callbacks execute on the transport worker, borrow their text
until return and must not block, throw, reenter Core or execute tools. Cancelled or
unknown calls are never automatically retried. Release the accumulator and Answer
when their terminal borrow transfers.

Supported Responses output: assistant output_text/refusal, reasoning and function
calls. Full ordered Items, including encrypted reasoning, bind to their assistant
frame and exact API/model/route. Unsupported output and changed selections are
explicit errors; callers choose a new conversation. No provider continuation is
included in canonical summary input. See [ADR-0098](../../docs/adr/0098-shared-model-protocol-and-preloaded-tools.md).
