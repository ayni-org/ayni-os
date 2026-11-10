#include "model_wire_fixture.h"
#include <ayni/model_messages.hpp>
#include <ayni/providers/model_protocol.h>
#include <gtest/gtest.h>
#include <memory>
#include <tuple>
#include <yyjson.h>

namespace {
using namespace ayni::providers::model_protocol;
ayni_string_v1 text(std::string_view value) {
    return {value.data(), uint32_t(value.size())};
}
std::string encode(const RequestOptions &options, ayni_string_v1 input) {
    Protocol codec;
    std::string result;
    EXPECT_EQ(codec.encode(options, input, result), AYNI_OK);
    auto measured = ayni::json::Writer::measuring();
    EXPECT_EQ(codec.write_request(measured, options, input), AYNI_OK);
    size_t size = 0;
    EXPECT_TRUE(measured.finish(size));
    EXPECT_EQ(size, result.size());
    std::string bounded(size, '\0');
    ayni::json::Writer writer(bounded.data(), bounded.size());
    EXPECT_EQ(codec.write_request(writer, options, input), AYNI_OK);
    EXPECT_TRUE(writer.finish(size));
    EXPECT_EQ(bounded, result);
    return result;
}
TEST(ModelProtocol, NativeAndBoundedEspEncodingShareTheExactChatWire) {
    // ADR-0055/0097: one codec, separate HTTP transports; no credential in protocol options.
    RequestOptions options;
    options.model = "fixture";
    const auto result = encode(options, text("你好"));
    auto *doc = yyjson_read(result.data(), result.size(), 0);
    ASSERT_NE(doc, nullptr);
    auto *root = yyjson_doc_get_root(doc);
    EXPECT_FALSE(yyjson_get_bool(yyjson_obj_get(root, "stream")));
    EXPECT_EQ(yyjson_arr_size(yyjson_obj_get(root, "tools")), ayni::model::native_function_count);
    yyjson_doc_free(doc);
}
TEST(ModelProtocol, CompleteToolHistoryPairsIdsAndSendsDefinitionsOncePerRequest) {
    // ADR-0085: protocol IDs pair messages; definitions are not added to history.
    char data[4096]{};
    ayni::model::Writer frame(data, sizeof(data));
    frame.prefix();
    frame.text(ayni::model::Role::user, text("show hi"));
    frame.calls(1);
    frame.function(
        {text("call-original"), text(ayni::model::call_function),
         text(
             R"({"binding_id":"screen.show_text","binding_version":1,"arguments":{"text":"hi"}})")});
    frame.tool(text("call-original"), text(R"({"status":0})"));
    ayni_string_v1 history{};
    ASSERT_TRUE(frame.finish(history));
    RequestOptions options;
    options.model = "fixture";
    const auto result = encode(options, history);
    const auto tools = result.find("\"tools\":");
    ASSERT_NE(tools, std::string::npos);
    EXPECT_EQ(result.find("\"tools\":", tools + 1), std::string::npos);
    EXPECT_NE(result.find("\"tool_call_id\":\"call-original\""), std::string::npos);
}
TEST(ModelProtocol, TextSummariesHaveNoToolsAndExplicitExtensionsCannotOverrideTheProtocol) {
    // ADR-0055: text mode has no tools; unsupported/ambiguous requests are not silently changed.
    RequestOptions options;
    options.model = "fixture";
    options.result_format = AYNI_CALL_RESULT_TEXT;
    options.extra_parameters = R"({"thinking":{"type":"disabled"}})";
    const auto result = encode(options, text("summarize"));
    EXPECT_EQ(result.find("\"tools\":"), std::string::npos);
    EXPECT_NE(result.find("\"thinking\""), std::string::npos);
    options.extra_parameters = R"({"messages":[]})";
    std::string rejected;
    EXPECT_EQ(Protocol{}.encode(options, text("summarize"), rejected), AYNI_INVALID_ARGUMENT);
    options.extra_parameters = {};
    options.max_output_tokens = 0;
    EXPECT_EQ(Protocol{}.encode(options, text("summarize"), rejected), AYNI_INVALID_ARGUMENT);
    options.max_output_tokens = 1;
    options.result_format = 0;
    EXPECT_EQ(Protocol{}.encode(options, text("summarize"), rejected), AYNI_INVALID_ARGUMENT);
}
TEST(ModelProtocol, CompleteAnswerRetainsReportedUsageWithOwnedPrivateViews) {
    // C-PROV-004/ADR-0095: optional service statistics are independent of the answer bytes.
    std::string response =
        R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"你好"}}],"usage":{"prompt_tokens":30,"completion_tokens":4,"total_tokens":34}})";
    auto answer = std::make_unique<Answer>();
    Protocol{}.decode(response.data(), response.size(), *answer);
    ASSERT_EQ(answer->result.status, AYNI_OK);
    EXPECT_EQ(answer->result.model_usage.kind, AYNI_MODEL_USAGE_REPORTED);
    EXPECT_EQ(answer->result.model_usage.input_tokens, 30U);
    response.assign(response.size(), 'x');
    EXPECT_EQ(std::string_view(answer->result.result.data, answer->result.result.size), "你好");
}
TEST(ModelProtocol, MalformedInnerArgumentsRemainAToolProposalForTheSdkError) {
    // ADR-0085: never guess/repair arguments, but preserve the protocol ID for a matching reply.
    std::string response =
        R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"call-1","type":"function","function":{"name":"ayni_call_capability","arguments":"{"}}]}}]})";
    auto answer = std::make_unique<Answer>();
    Protocol{}.decode(response.data(), response.size(), *answer);
    ASSERT_EQ(answer->result.status, AYNI_OK);
    ASSERT_EQ(answer->result.tool_call_count, 1U);
    EXPECT_EQ(answer->calls[0].input_kind, AYNI_TOOL_INPUT_INVALID_ARGUMENTS);
    EXPECT_EQ(std::string_view(answer->calls[0].input.invalid_arguments.data,
                               answer->calls[0].input.invalid_arguments.size),
              "{");
}
TEST(ModelFrames, ExplicitReplyEncodingKeepsOrdinaryTextSeparateFromProviderContinuation) {
    // Shared protocol plan: encoding is selected by configuration, never by model text.
    char bytes[4096]{};
    ayni::model::Writer writer(bytes, sizeof(bytes));
    const ayni::model::Continuation continuation{text("openai.responses.output.v1"), text("[]")};
    ayni::model::write_reply_frame(writer, text("你好"), continuation);
    ayni_string_v1 encoded{}, answer{};
    ayni::model::Continuation decoded{};
    ASSERT_TRUE(writer.finish(encoded));
    ASSERT_EQ(
        ayni::model::read_reply_frame(encoded, ayni::model::ReplyEncoding::framed, answer, decoded),
        AYNI_OK);
    EXPECT_EQ(std::string_view(answer.data, answer.size), "你好");
    EXPECT_EQ(std::string_view(decoded.data.data, decoded.data.size), "[]");
    ASSERT_EQ(
        ayni::model::read_reply_frame(encoded, ayni::model::ReplyEncoding::text, answer, decoded),
        AYNI_OK);
    EXPECT_EQ(answer.size, encoded.size); // A literal marker in legacy text stays literal text.
    EXPECT_EQ(decoded.codec.size, 0U);
}
TEST(ModelFrames, ContinuationStaysAttachedToItsToolGroupAndChatRequiresExplicitHandling) {
    // Shared protocol plan: continuation is not a separate message that compaction can split.
    char bytes[4096]{};
    ayni::model::Writer writer(bytes, sizeof(bytes));
    writer.prefix();
    writer.calls(1, {}, {text("openai.responses.output.v1"), text("[]")});
    writer.function({text("call-1"), text(ayni::model::call_function), text("{}")});
    writer.tool(text("call-1"), text("{}"));
    ayni_string_v1 encoded{};
    ASSERT_TRUE(writer.finish(encoded));
    auto rejected = ayni::json::Writer::measuring();
    EXPECT_EQ(ayni::model::write_messages(rejected, encoded), AYNI_UNSUPPORTED);
    auto canonical = ayni::json::Writer::measuring();
    EXPECT_EQ(ayni::model::write_messages(canonical, encoded, ayni::model::Completeness::complete,
                                          ayni::model::Continuations::canonical),
              AYNI_OK);
    ayni::model::Reader reader(encoded);
    ayni::model::Message message;
    ASSERT_TRUE(reader.next(message));
    EXPECT_EQ(message.role, ayni::model::Role::calls);
    EXPECT_EQ(message.continuation.data.size, 2U);
    ASSERT_TRUE(reader.next(message));
    EXPECT_EQ(message.role, ayni::model::Role::tool);
    EXPECT_EQ(message.continuation.data.size, 0U);
    EXPECT_TRUE(reader.finished());
}
TEST(ModelFrames, RequestOnlyDefinitionsReplaceTheFixedToolsWithoutEnteringMessages) {
    // Shared protocol plan: a current request has one tools array; history holds only messages.
    char history_bytes[4096]{}, request_bytes[8192]{}, json_bytes[8192]{};
    ayni::model::Writer history(history_bytes, sizeof(history_bytes));
    history.prefix();
    history.text(ayni::model::Role::user, text("hi"));
    ayni_string_v1 messages{}, request{};
    ASSERT_TRUE(history.finish(messages));
    ayni::model::Writer envelope(request_bytes, sizeof(request_bytes));
    ayni::model::write_request_frame(envelope, text("[]"), messages);
    ASSERT_TRUE(envelope.finish(request));
    ayni::json::Writer out(json_bytes, sizeof(json_bytes));
    ASSERT_EQ(ayni::model::write_context(out, request, ayni::model::Mode::assistant), AYNI_OK);
    size_t size = 0;
    ASSERT_TRUE(out.finish(size));
    const std::string_view json(json_bytes, size);
    EXPECT_NE(json.find("\"tools\":[]"), std::string_view::npos);
    EXPECT_EQ(json.find("ayni_list_capabilities"), std::string_view::npos);
    EXPECT_EQ(json.find(ayni::model::request_prefix), std::string_view::npos);
}
TEST(ModelFrames, EachContinuationSectionMustBeIndividuallyValidUtf8) {
    // The existing take() validator owns the boundary; whole-body UTF-8 is insufficient.
    const std::string glyph = "界";
    char bytes[4096]{};
    ayni::model::Writer writer(bytes, sizeof(bytes));
    writer.prefix();
    writer.text(ayni::model::Role::assistant, {glyph.data(), 1},
                {{glyph.data() + 1, 1}, {glyph.data() + 2, 1}});
    ayni_string_v1 encoded{};
    ASSERT_TRUE(writer.finish(encoded));
    ayni::model::Reader reader(encoded);
    ayni::model::Message message;
    EXPECT_FALSE(reader.next(message));
    EXPECT_FALSE(reader.finished());
}
} // namespace

namespace {
using namespace ayni::test::model_wire;
class ModelMatrix : public ::testing::TestWithParam<std::tuple<Api, bool, bool>> {};
TEST_P(ModelMatrix, TextToolsUsageAndWireProjectionAgree) {
    // ADR-0098: two APIs x streaming x text/tools, using byte cuts through UTF-8 and CRLF.
    const auto [api, streaming, tools] = GetParam();
    RequestOptions options;
    options.model = "fixture";
    options.api = api;
    options.stream = streaming;
    options.reply_encoding = ayni::model::ReplyEncoding::framed;
    options.continuation_scope = "fixture.route";
    options.result_format = tools ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT;
    auto answer = std::make_unique<Answer>();
    std::string preview;
    if (streaming) {
        const auto bytes = stream_answer(api, tools);
        Stream stream(options, 65536, {&preview, [](void *context, std::string_view value) {
                                           static_cast<std::string *>(context)->append(value);
                                       }});
        for (const char &byte : bytes) {
            ASSERT_EQ(stream.feed({&byte, 1}), AYNI_OK);
        }
        stream.finish(*answer);
        EXPECT_EQ(preview, "你好");
    } else {
        std::string bytes =
            api == Api::responses ? responses_answer(tools)
            : tools
                ? R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":"你好","tool_calls":[{"id":"call_1","type":"function","function":{"name":"ayni_call_capability","arguments":"{}"}}]}}],"usage":{"prompt_tokens":30,"completion_tokens":4,"total_tokens":34}})"
                : R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"你好"}}],"usage":{"prompt_tokens":30,"completion_tokens":4,"total_tokens":34}})";
        Protocol{}.decode(options, bytes.data(), bytes.size(), *answer);
    }
    ASSERT_EQ(answer->result.status, AYNI_OK);
    EXPECT_EQ(answer->result.tool_call_count, tools ? 1U : 0U);
    EXPECT_EQ(std::string_view(answer->result.result.data, answer->result.result.size), "你好");
    EXPECT_EQ(answer->result.model_usage.kind, AYNI_MODEL_USAGE_REPORTED);
    EXPECT_EQ(answer->result.model_usage.input_tokens, 30U);
    std::string stored;
    ASSERT_EQ(encode_answer(options, *answer, stored), AYNI_OK);
    std::array<char, 16384> history{};
    ayni::model::Writer writer(history.data(), history.size());
    writer.prefix();
    writer.text(ayni::model::Role::user, text("hi"));
    const ayni::model::Continuation state =
        answer->continuation.empty()
            ? ayni::model::Continuation{}
            : ayni::model::Continuation{text(api == Api::responses ? "openai.responses.output.v1"
                                                                   : "openai.chat.selection.v1"),
                                        text(answer->continuation)};
    if (tools) {
        writer.calls(1, answer->result.result, state);
        writer.function({text("call_1"), text(ayni::model::call_function), text("{}")});
        writer.tool(text("call_1"), text("result"));
    } else {
        writer.text(ayni::model::Role::assistant, answer->result.result, state);
    }
    ayni_string_v1 framed{};
    ASSERT_TRUE(writer.finish(framed));
    const auto request = encode(options, framed);
    auto *doc = yyjson_read(request.data(), request.size(), 0);
    ASSERT_NE(doc, nullptr);
    auto *root = yyjson_doc_get_root(doc);
    EXPECT_EQ(yyjson_get_bool(yyjson_obj_get(root, "stream")), streaming);
    if (api == Api::responses) {
        EXPECT_NE(yyjson_obj_get(root, "input"), nullptr);
        EXPECT_EQ(yyjson_obj_get(root, "messages"), nullptr);
        EXPECT_FALSE(yyjson_get_bool(yyjson_obj_get(root, "store")));
        EXPECT_EQ(yyjson_obj_get(root, "previous_response_id"), nullptr);
        if (tools) {
            EXPECT_NE(request.find("fc_item_1"), std::string::npos);
        }
        if (tools) {
            EXPECT_NE(request.find("\"call_id\":\"call_1\",\"output\":\"result\""),
                      std::string::npos);
            EXPECT_NE(request.find("opaque-fixture"), std::string::npos);
        }
    }
    yyjson_doc_free(doc);
    auto measured = ayni::json::Writer::measuring();
    ASSERT_EQ(
        Protocol{}.write_context(measured, options, framed,
                                 tools ? ayni::model::Mode::assistant : ayni::model::Mode::text),
        AYNI_OK);
    ayni::model::TokenEstimate estimate;
    ASSERT_EQ(ayni::model::estimate_input(
                  Protocol::projection(options), framed,
                  tools ? ayni::model::Mode::assistant : ayni::model::Mode::text, estimate),
              AYNI_OK);
    EXPECT_EQ(estimate.characters, measured.characters());
}
INSTANTIATE_TEST_SUITE_P(SharedWire, ModelMatrix,
                         ::testing::Combine(::testing::Values(Api::chat_completions,
                                                              Api::responses),
                                            ::testing::Bool(), ::testing::Bool()));
TEST(ModelProtocol, ResponsesContinuationRejectsChangedModelRouteAndApi) {
    // ADR-0098: stateless encrypted reasoning is bound to its exact request selection.
    RequestOptions options;
    options.model = "fixture";
    options.api = Api::responses;
    options.reply_encoding = ayni::model::ReplyEncoding::framed;
    options.continuation_scope = "route";
    auto answer = std::make_unique<Answer>();
    auto response = responses_answer(false);
    Protocol{}.decode(options, response.data(), response.size(), *answer);
    ASSERT_EQ(answer->result.status, AYNI_OK);
    char bytes[4096]{};
    ayni::model::Writer writer(bytes, sizeof(bytes));
    writer.prefix();
    writer.text(ayni::model::Role::assistant, answer->result.result,
                {text("openai.responses.output.v1"), text(answer->continuation)});
    ayni_string_v1 history{};
    ASSERT_TRUE(writer.finish(history));
    std::string out;
    options.model = "changed";
    EXPECT_EQ(Protocol{}.encode(options, history, out), AYNI_REVISION_CONFLICT);
    options.model = "fixture";
    options.continuation_scope = "changed";
    EXPECT_EQ(Protocol{}.encode(options, history, out), AYNI_REVISION_CONFLICT);
    options.api = Api::chat_completions;
    EXPECT_EQ(Protocol{}.encode(options, history, out), AYNI_UNSUPPORTED);
}
TEST(ModelProtocol, ExplicitModernChatTokenLimitDoesNotDependOnModelName) {
    // ADR-0098: select the field explicitly; extensions cannot replace owned protocol fields.
    RequestOptions options;
    options.model = "any-model";
    options.chat_token_limit = ChatTokenLimit::max_completion_tokens;
    const auto request = encode(options, text("hi"));
    EXPECT_NE(request.find("\"max_completion_tokens\":1024"), std::string::npos);
    EXPECT_EQ(request.find("\"max_tokens\":"), std::string::npos);
}
} // namespace

namespace {
TEST(ModelProtocol, ResponsesArgumentDeltasFinishWithOrderedItemsAndSemanticObjectEquality) {
    // ADR-0098: item order matters; object member order does not. arguments.done is not terminal.
    RequestOptions options;
    options.model = "fixture";
    options.api = Api::responses;
    options.reply_encoding = ayni::model::ReplyEncoding::framed;
    options.continuation_scope = "route";
    Stream stream(options, 16384);
    ASSERT_EQ(
        stream.feed(sse_event(
            "response.output_item.added",
            R"({"type":"response.output_item.added","output_index":0,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"ayni_call_capability","arguments":""}})")),
        AYNI_OK);
    for (const auto piece : {"{", "}"}) {
        const auto event =
            std::string(
                R"({"type":"response.function_call_arguments.delta","output_index":0,"item_id":"fc_1","delta":")") +
            piece + R"("})";
        ASSERT_EQ(stream.feed(sse_event("response.function_call_arguments.delta", event)), AYNI_OK);
    }
    ASSERT_EQ(
        stream.feed(sse_event(
            "response.function_call_arguments.done",
            R"({"type":"response.function_call_arguments.done","output_index":0,"item_id":"fc_1","arguments":"{}"})")),
        AYNI_OK);
    auto pending = std::make_unique<Answer>();
    stream.finish(*pending);
    EXPECT_EQ(pending->result.outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(pending->result.status, AYNI_RESULT_UNKNOWN);
    ASSERT_EQ(
        stream.feed(sse_event(
            "response.output_item.done",
            R"({"type":"response.output_item.done","output_index":0,"item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"ayni_call_capability","arguments":"{}"}})")),
        AYNI_OK);
    ASSERT_EQ(
        stream.feed(sse_event(
            "response.completed",
            R"({"type":"response.completed","response":{"status":"completed","output":[{"arguments":"{}","name":"ayni_call_capability","call_id":"call_1","id":"fc_1","type":"function_call"}]}})")),
        AYNI_OK);
    stream.finish(*pending);
    ASSERT_EQ(pending->result.status, AYNI_OK);
    EXPECT_EQ(pending->result.tool_call_count, 1U);
}
TEST(ModelProtocol, OptionalAmbiguousUsageNeverDiscardsAnOtherwiseCompletedStream) {
    // C-PROV-004/ADR-0095: accounting ambiguity stays separate from model completion.
    for (const auto api : {Api::chat_completions, Api::responses}) {
        RequestOptions options;
        options.model = "fixture";
        options.api = api;
        options.reply_encoding = ayni::model::ReplyEncoding::framed;
        options.continuation_scope = "route";
        auto wire = stream_answer(api, false);
        const std::string key =
            api == Api::responses ? "\"input_tokens\":30" : "\"prompt_tokens\":30";
        const auto at = wire.find(key);
        ASSERT_NE(at, std::string::npos);
        wire.insert(at + key.size(), "," + key);
        Stream stream(options, 32768);
        ASSERT_EQ(stream.feed(wire), AYNI_OK);
        auto answer = std::make_unique<Answer>();
        stream.finish(*answer);
        ASSERT_EQ(answer->result.status, AYNI_OK);
        EXPECT_EQ(answer->result.model_usage.kind, AYNI_MODEL_USAGE_INVALID);
    }
}
TEST(ModelProtocol, ResponsesRefusalUsesVisibleTextAndRetainsOriginalItem) {
    // ADR-0098: refusal is a native assistant result, preserved for the next request.
    RequestOptions options;
    options.model = "fixture";
    options.api = Api::responses;
    options.reply_encoding = ayni::model::ReplyEncoding::framed;
    options.continuation_scope = "route";
    std::string wire =
        R"({"status":"completed","output":[{"type":"message","id":"m","role":"assistant","status":"completed","content":[{"type":"refusal","refusal":"Cannot do that."}]}]})";
    auto answer = std::make_unique<Answer>();
    Protocol{}.decode(options, wire.data(), wire.size(), *answer);
    ASSERT_EQ(answer->result.status, AYNI_OK);
    EXPECT_EQ(std::string_view(answer->result.result.data, answer->result.result.size),
              "Cannot do that.");
    EXPECT_NE(answer->continuation.find("\"type\":\"refusal\""), std::string::npos);
}
} // namespace
