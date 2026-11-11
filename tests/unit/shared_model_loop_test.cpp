#include "strategies_fixture.h"
#include <ayni/providers/model_protocol.h>
#include <gtest/gtest.h>
#include <memory>
#include <yyjson.h>

namespace {
using namespace ayni::test;
using namespace ayni::providers::model_protocol;
class SharedModelLoop : public StrategiesFixture, public ::testing::WithParamInterface<Api> {
  protected:
    RequestOptions options;
    void SetUp() override {
        options.model = "fixture";
        options.api = GetParam();
        options.reply_encoding = ayni::model::ReplyEncoding::framed;
        options.continuation_scope = "loop.fixture";
        limits = {12000, 1000, 2000, 2000};
        configure_loop = [&](policies::ToolLoopConfig &config) {
            config.tool_exposure = policies::ToolExposure::preloaded;
            config.reply_encoding = config.summary_reply_encoding =
                ayni::model::ReplyEncoding::framed;
            config.projection = Protocol::projection(options);
            config.clock.timezone =
                text("UTC0"); // Pending-group budgeting includes a real clock envelope.
        };
        StrategiesFixture::SetUp();
    }
    void answer(uint32_t at, bool tools, const char *content = "done") {
        ASSERT_LT(at, pending.size());
        std::string wire;
        if (options.api == Api::responses) {
            wire =
                R"({"status":"completed","output":[{"type":"reasoning","id":"rs_loop","summary":[],"encrypted_content":"opaque-loop-secret"},)";
            wire +=
                tools
                    ? R"({"type":"function_call","id":"fc_loop","call_id":"call_loop","name":"ayni_call_capability","arguments":"{\"binding_id\":\"echo\",\"binding_version\":1,\"arguments\":\"tool body\"}"}])"
                    : std::string(
                          R"({"type":"message","id":"msg_loop","role":"assistant","status":"completed","content":[{"type":"output_text","text":")") +
                          content + R"("}]}])";
            wire += R"(,"usage":{"input_tokens":30,"output_tokens":4,"total_tokens":34}})";
        } else {
            wire =
                tools
                    ? R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":null,"tool_calls":[{"id":"call_loop","type":"function","function":{"name":"ayni_call_capability","arguments":"{\"binding_id\":\"echo\",\"binding_version\":1,\"arguments\":\"tool body\"}"}}]}}]})"
                    : std::string(
                          R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":")") +
                          content + R"("}}]})";
        }
        auto decoded = std::make_unique<Answer>();
        Protocol{}.decode(options, wire.data(), wire.size(), *decoded);
        ASSERT_EQ(decoded->result.status, AYNI_OK);
        std::string stored;
        ASSERT_EQ(encode_answer(options, *decoded, stored), AYNI_OK);
        ayni_body_ref_v1 ref{};
        ASSERT_EQ(bodies.store()->write(
                      bodies.store()->context, pending[at].operation,
                      {reinterpret_cast<const uint8_t *>(stored.data()), uint32_t(stored.size())},
                      &ref),
                  AYNI_OK);
        auto result = decoded->result;
        result.attempt = pending[at].attempt;
        result.result = {};
        result.tool_calls = nullptr;
        result.body = ref;
        ASSERT_EQ(ayni_provider_post(pending[at].completion, &result), AYNI_OK);
        pending[at].ended = true;
        pump();
    }
    void check_request(const std::string &body, bool has_tools = true) {
        std::string wire;
        auto selected = options;
        selected.result_format = has_tools ? AYNI_CALL_RESULT_TOOL_CALLS : AYNI_CALL_RESULT_TEXT;
        ASSERT_EQ(Protocol{}.encode(selected, {body.data(), uint32_t(body.size())}, wire), AYNI_OK);
        auto *doc = yyjson_read(wire.data(), wire.size(), 0);
        ASSERT_NE(doc, nullptr);
        auto *tools = yyjson_obj_get(yyjson_doc_get_root(doc), "tools");
        EXPECT_EQ(yyjson_arr_size(tools), has_tools ? 1U : 0U);
        if (has_tools) {
            auto *function = yyjson_arr_get_first(tools);
            if (options.api == Api::chat_completions) {
                function = yyjson_obj_get(function, "function");
            }
            EXPECT_TRUE(
                yyjson_equals_str(yyjson_obj_get(function, "name"), "ayni_call_capability"));
            auto *branches = yyjson_obj_get(yyjson_obj_get(function, "parameters"), "oneOf");
            EXPECT_EQ(yyjson_arr_size(branches), 3U); // echo plus operation controls and wait.
            EXPECT_NE(wire.find("Native input."), std::string::npos);
            EXPECT_EQ(wire.find("ayni_list_capabilities"), std::string::npos);
            EXPECT_EQ(wire.find("ayni_inspect_capability"), std::string::npos);
        }
        yyjson_doc_free(doc);
    }
};
TEST_P(SharedModelLoop, PreloadedToolsUseTheRealLoopAndNeverAccumulateInHistory) {
    // ADR-0098: model -> authorized business call -> tool result -> model, without directory calls.
    const auto task = start();
    ASSERT_EQ(pending.size(), 1U);
    check_request(pending[0].text);
    answer(0, true);
    ASSERT_EQ(pending.size(), 2U);
    EXPECT_EQ(pending[1].capability, "echo");
    post(1, text("actual echo result"));
    pump();
    ASSERT_EQ(pending.size(), 3U);
    EXPECT_EQ(pending[2].capability, "model");
    check_request(pending[2].text);
    answer(2, false);
    EXPECT_EQ(work.task(task).lifecycle, AYNI_TASK_COMPLETED);
    finish(task);
    EXPECT_EQ(rounds_of(conversation).back().answer, "done");
    auto history = text_of(conversation.history);
    EXPECT_EQ(history.find(ayni::model::request_prefix), std::string::npos);
    EXPECT_EQ(history.find("Echo input."), std::string::npos);
    EXPECT_EQ(history.find(ayni::model::reply_prefix), std::string::npos);
    if (GetParam() == Api::responses) {
        EXPECT_NE(history.find("opaque-loop-secret"), std::string::npos);
    }
    const auto next = start("chat", "next question");
    ASSERT_EQ(pending.size(), 4U);
    check_request(pending[3].text);
    answer(3, false);
    finish(next);
    EXPECT_EQ(rounds_of(conversation).back().answer, "done");
}
TEST_P(SharedModelLoop, CanonicalCompactionDoesNotDiscloseOpaqueContinuationToSummary) {
    // ADR-0098: original complete groups stay retained until summaries succeed; opaque provider
    // state never appears in a summary request, while tool pairing remains in protected history.
    uint64_t task = 0;
    const std::string input(2500, 'q');
    for (uint32_t turn = 0; turn < 12; ++turn) {
        task = start("chat", input.c_str());
        ASSERT_FALSE(pending.empty());
        const auto at = uint32_t(pending.size() - 1);
        if (pending[at].capability == "summary") {
            break;
        }
        answer(at, false);
        finish(task);
    }
    ASSERT_EQ(pending.back().capability, "summary");
    uint32_t chunks = 0;
    while (pending.back().capability == "summary" && chunks++ < 16) {
        EXPECT_EQ(pending.back().text.find("opaque-loop-secret"), std::string::npos);
        EXPECT_EQ(pending.back().text.find("encrypted_content"), std::string::npos);
        check_request(pending.back().text, false);
        answer(uint32_t(pending.size() - 1), false, "small summary");
    }
    ASSERT_EQ(pending.back().capability, "model");
    check_request(pending.back().text);
    answer(uint32_t(pending.size() - 1), false);
    finish(task);
    EXPECT_EQ(text_of(conversation.summary), "small summary");
}
INSTANTIATE_TEST_SUITE_P(SharedApis, SharedModelLoop,
                         ::testing::Values(Api::chat_completions, Api::responses));
} // namespace

TEST(PreloadedConfiguration, RequiresTheServingProtocolsProjection) {
    // ADR-0098: complete definitions must use the same writer for budget and egress.
    policies::ToolLoopConfig config;
    config.identity = {text("loop"), 1, nullptr, 0};
    config.model = {
        AYNI_POLICY_REQUEST_MODEL, text("model"), 1, {sizeof(ayni_scope_v1), 0, nullptr}, {}};
    config.summary = config.model;
    config.summary.binding_id = text("summary");
    config.limits = {5000, 1000, 1000, 1000};
    ayni::runtime::MemoryBodies bodies;
    config.bodies = bodies.store();
    config.tool_exposure = policies::ToolExposure::preloaded;
    policies::ToolLoop loop;
    EXPECT_EQ(loop.configure(config, loop_workspace()), AYNI_INVALID_ARGUMENT);
}
