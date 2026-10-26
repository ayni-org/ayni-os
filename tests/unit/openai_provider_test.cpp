// ADR-0055: normal protocol behavior with loopback-only HTTP fixtures.
#include "openai_fixture.h"

namespace {
using namespace ayni::test;
using OpenaiProvider = OpenaiFixture;
const char *text_reply =
    R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"hello 世界"}}]})";
std::string tool_reply(const std::string &arguments, const std::string &name = "tool_0") {
    std::string escaped;
    for (char c : arguments) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += c;
    }
    return "{\"choices\":[{\"finish_reason\":\"tool_calls\",\"message\":{\"role\":\"assistant\","
           "\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":"
           "{\"name\":\"" +
           name + "\",\"arguments\":\"" + escaped + "\"}}]}}]}";
}
TEST_F(OpenaiProvider, text_completes_through_public_abi_and_declares_remote_effects) {
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(reply, "hello 世界");
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_EQ(descriptor.capabilities->contract.effects,
              AYNI_CALL_EFFECT_EGRESS | AYNI_CALL_EFFECT_BILLING);
    EXPECT_EQ(descriptor.capabilities->contract.query, AYNI_CALL_QUERY_NONE);
    EXPECT_EQ(descriptor.query, nullptr);
    const auto request = server->request();
    EXPECT_NE(request.find("POST /chat/completions HTTP/1.1"), std::string::npos);
    EXPECT_NE(request.find("\"stream\":false"), std::string::npos);
    EXPECT_NE(request.find("\"minimum\":5,\"maximum\":10"), std::string::npos);
    EXPECT_NE(request.find("\"description\":\"second\""), std::string::npos);
    EXPECT_NE(request.find("\"enum\":[\"bright\",\"calm\"]"), std::string::npos);
    EXPECT_NE(request.find("Authorization: Bearer fixture-only-key"), std::string::npos);
}
TEST_F(OpenaiProvider, offered_function_becomes_exact_typed_values) {
    start(tool_reply(R"({"field_1":7,"field_2":"calm","text":"hello"})"));
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(seen_result.tool_call_count, 1U);
    EXPECT_EQ(proposal, "show|hello|7second|calm");
    EXPECT_TRUE(reply.empty());
}
TEST_F(OpenaiProvider, malformed_json_is_model_output_failure) {
    start("{broken");
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
}
TEST_F(OpenaiProvider, oversized_http_body_is_bounded_model_output_failure) {
    start(std::string(2048, 'x'), 200, 0, 1024);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
}
TEST_F(OpenaiProvider, timeout_returns_borrow_without_retrying_unknown_execution) {
    start(text_reply, 200, 1000, 4096, 100);
    const auto goal = create();
    wait_request();
    const auto call_id = work.task(work.goal(goal).last_task_id).logical_call_id;
    // Freeze the Kernel clock so libcurl's HTTP deadline, not Core cancellation,
    // is the only timeout source in this protocol fixture (ADR-0055).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (provider.pending() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(provider.pending(), 0U);
    wait_done();
    ayni_call_info_v1 call{};
    call.struct_size = sizeof(call);
    ASSERT_EQ(ayni_get_call(work.kernel(), call_id, &call), AYNI_OK);
    EXPECT_EQ(call.outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(call.status, AYNI_RESULT_UNKNOWN);
    EXPECT_EQ(call.reason, AYNI_REASON_NONE);
    EXPECT_EQ(call.cancel_result, 0U);
    EXPECT_EQ(call.effect, AYNI_CALL_EFFECT_UNKNOWN);
    EXPECT_EQ(provider.pending(), 0U);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_EQ(work.task(work.goal(goal).last_task_id).lifecycle, AYNI_TASK_PAUSED);
}
TEST_F(OpenaiProvider, shutdown_cancels_http_and_drains_the_borrow) {
    start(text_reply, 200, 1000);
    const auto goal = create();
    wait_request();
    const auto call_id = work.task(work.goal(goal).last_task_id).logical_call_id;
    EXPECT_EQ(provider.close(), AYNI_BUSY);
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    wait_done();
    ayni_call_info_v1 call{};
    call.struct_size = sizeof(call);
    ASSERT_EQ(ayni_get_call(work.kernel(), call_id, &call), AYNI_OK);
    EXPECT_EQ(call.cancel_result, AYNI_CALL_CANCEL_UNKNOWN);
    EXPECT_EQ(call.outcome, AYNI_CALL_ABANDONED);
    EXPECT_EQ(call.effect, AYNI_CALL_EFFECT_UNKNOWN);
    EXPECT_EQ(provider.pending(), 0U);
    EXPECT_EQ(server->requests(), 1U);
}

class OpenaiHttpError : public OpenaiProvider,
                        public ::testing::WithParamInterface<std::pair<int, ayni_status_t>> {};
TEST_P(OpenaiHttpError, classifies_status_without_posting_vendor_error_or_retrying) {
    start(R"({"error":{"message":"private vendor detail"}})", GetParam().first);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, GetParam().second);
    EXPECT_TRUE(reply.empty());
    EXPECT_EQ(server->requests(), 1U);
}
INSTANTIATE_TEST_SUITE_P(
    Status, OpenaiHttpError,
    ::testing::Values(std::make_pair(400, AYNI_INVALID_ARGUMENT), std::make_pair(401, AYNI_DENIED),
                      std::make_pair(403, AYNI_DENIED), std::make_pair(404, AYNI_NOT_FOUND),
                      std::make_pair(408, AYNI_EXPIRED), std::make_pair(429, AYNI_BUSY),
                      std::make_pair(500, AYNI_NOT_READY), std::make_pair(503, AYNI_NOT_READY)));

class OpenaiArguments : public OpenaiProvider,
                        public ::testing::WithParamInterface<const char *> {};
TEST_P(OpenaiArguments, invalid_arguments_fail_before_kernel_receipt) {
    start(tool_reply(GetParam()));
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
    EXPECT_EQ(seen_result.tool_call_count, 0U);
}
INSTANTIATE_TEST_SUITE_P(
    Arguments, OpenaiArguments,
    ::testing::Values(R"({"field_1":11,"field_2":"calm","text":"hi"})",
                      R"({"field_1":7.5,"field_2":"calm","text":"hi"})",
                      R"({"field_1":18446744073709551615,"field_2":"calm","text":"hi"})",
                      R"({"field_1":7,"field_2":"other","text":"hi"})",
                      R"({"field_1":7,"text":"hi"})",
                      R"({"field_1":7,"field_1":8,"field_2":"calm","text":"hi"})",
                      R"({"field_1":7,"field_2":"calm","text":"hi","extra":true})",
                      R"({"field_1":7,"field_2":"calm","text":null})", "{}", "broken"));
TEST_F(OpenaiProvider, unknown_tool_is_model_output_failure) {
    start(tool_reply(R"({"field_1":7,"field_2":"calm","text":"hi"})", "unknown"));
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
}
TEST_F(OpenaiProvider, body_over_tool_limit_is_invalid_argument) {
    start(tool_reply("{\"field_1\":7,\"field_2\":\"calm\",\"text\":\"" + std::string(65, 'x') +
                     "\"}"));
    create();
    wait_done();
    EXPECT_EQ(seen_result.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
}
TEST_F(OpenaiProvider, redirect_does_not_send_credentials_to_another_server) {
    LoopbackHttp redirected(text_reply);
    start("", 302, 0, 4096, 1500, "Location: " + redirected.url() + "/chat/completions\r\n");
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(redirected.requests(), 0U);
}
// C-CALL-014: cardinality and encoded bytes are separate limits. The 8-call
// case reaches provider_post, whose refusal must be followed by a FAILED receipt.
class OpenaiListLimit : public OpenaiProvider, public ::testing::WithParamInterface<unsigned> {};
TEST_P(OpenaiListLimit, list_limits_return_capacity_and_release_the_borrow) {
    const auto one = tool_reply(R"({"field_1":7,"field_2":"calm","text":"hi"})");
    const auto begin = one.find("[{\"id\"") + 1;
    const auto end = one.rfind("]}}]}");
    const auto call = one.substr(begin, end - begin);
    std::string response = one.substr(0, begin);
    for (unsigned i = 0; i < GetParam(); ++i) {
        if (i) {
            response += ',';
        }
        response += call;
    }
    response += one.substr(end);
    start(response, 200, 0, 32768);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
    EXPECT_EQ(provider.pending(), 0U);
}
INSTANTIATE_TEST_SUITE_P(Limits, OpenaiListLimit, ::testing::Values(8U, 9U, 65U));
TEST_F(OpenaiProvider, deep_json_is_rejected_before_decoding) {
    start("{\"extra\":" + std::string(8, '[') + "0" + std::string(8, ']') + "}");
    create();
    wait_done();
    EXPECT_EQ(seen_result.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
}
// ADR-0055: never promote a truncated choice or ambiguous envelope to success.
class OpenaiEnvelope : public OpenaiProvider, public ::testing::WithParamInterface<const char *> {};
TEST_P(OpenaiEnvelope, invalid_envelope_is_model_output_failure) {
    start(GetParam());
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
}
INSTANTIATE_TEST_SUITE_P(
    Envelope, OpenaiEnvelope,
    ::testing::Values(
        R"({"choices":[{"finish_reason":"length","message":{"role":"assistant","content":"partial"}}]})",
        R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"a","content":"b"}}]})",
        R"({"choices":[]})",
        R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"hi","tool_calls":{}}}]})"));
TEST_F(OpenaiProvider, depth_eight_is_accepted_with_bounded_ignored_metadata) {
    auto response = std::string(text_reply);
    response.pop_back();
    response += ",\"extra\":" + std::string(7, '[') + "0" + std::string(7, ']') + "}";
    start(response);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
}
TEST_F(OpenaiProvider, terminal_slot_is_reusable_for_a_new_attempt) {
    start(text_reply);
    create();
    wait_done();
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    seen_result = {};
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(server->requests(), 2U);
}
TEST_F(OpenaiProvider, full_provider_queue_preserves_the_accepted_request) {
    start(text_reply, 200, 200);
    const auto first = create();
    wait_request();
    const auto second = create();
    pump(200);
    EXPECT_EQ(work.task(work.goal(second).last_task_id).lifecycle, AYNI_TASK_FAILED);
    seen_result = {};
    wait_done();
    EXPECT_EQ(work.task(work.goal(first).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(server->requests(), 1U);
}
TEST(OpenaiConfiguration, rejects_unsafe_urls_and_missing_key_without_starting_io) {
    Config config;
    config.model = "fixture";
    config.key_environment = "AYNI_PROTOCOL_MISSING_KEY";
    for (const char *url :
         {"http://example.com", "file:///tmp/key", "https://user:pass@example.com",
          "https://example.com?key=value", "https://example.com#fragment"}) {
        Provider provider;
        config.base_url = url;
        EXPECT_EQ(provider.configure(config), AYNI_INVALID_ARGUMENT);
    }
    Provider provider;
    config.base_url = "https://example.com";
    config.provider_id = "Uppercase";
    EXPECT_EQ(provider.configure(config), AYNI_INVALID_ARGUMENT);
    config.provider_id = "valid";
    EXPECT_EQ(provider.configure(config), AYNI_NOT_READY);
    EXPECT_EQ(provider.pending(), 0U);
}
} // namespace
