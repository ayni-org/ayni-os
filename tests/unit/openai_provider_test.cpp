#include "workspace_fixture.h"
// ADR-0055: normal protocol behavior with loopback-only HTTP fixtures.
#include "../../providers/openai_compat/protocol.h"
#include "catalog/capability/business_encoding.h"
#include "catalog/capability/capability_registry.h"
#include "openai_fixture.h"
#include <ayni/detail/tool_usage.hpp>

namespace {
using namespace ayni::test;
using OpenaiProvider = OpenaiFixture;
const char *text_reply =
    R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"hello 世界"}}]})";
std::string tool_reply(const std::string &arguments,
                       const std::string &name = "ayni_call_capability") {
    std::string escaped;
    const auto native_arguments =
        std::string(R"({"binding_id":"show","binding_version":1,"arguments":)") + arguments + "}";
    for (char c : native_arguments) {
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
    for (auto name : {"ayni_list_capabilities", "ayni_inspect_capability", "ayni_call_capability",
                      "ayni_shell"}) {
        EXPECT_NE(request.find(name), std::string::npos);
    }
    EXPECT_EQ(request.find(usage_purpose), std::string::npos);
    EXPECT_EQ(request.find("How long to display the text."), std::string::npos);
    EXPECT_EQ(usage_lookups, 1U); // Trusted startup adoption, never a submit callback.
    EXPECT_NE(request.find("Authorization: Bearer fixture-only-key"), std::string::npos);
    EXPECT_NE(request.find(R"("messages":[{"role":"user","content":"say hello"}])"),
              std::string::npos);
}
// ADR-0085: token counts come from the serving model, including its formatting.
// ADR-0096: neither direction is bounded by a Core byte limit. The request body and the answer
// travel through the body store; Core holds only their references.
TEST_F(OpenaiProvider, request_and_answer_over_200_kib_travel_by_reference) {
    constexpr size_t large = 210 * 1024;
    prompt.assign(large, 'q');
    prompt_by_reference = true;
    const std::string answer(large, 'a');
    start(R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":")" +
              answer + R"("}}]})",
          200, 0, 4096);
    create();
    wait_done(10);
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_NE(seen_result.body.id, 0U);
    EXPECT_EQ(seen_result.body.size, large);
    EXPECT_EQ(seen_result.text.size, 0U);
    EXPECT_EQ(reply, answer);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_NE(server->request().find(prompt), std::string::npos);
}
// The Provider verifies a request body against its reference before any network I/O.
TEST_F(OpenaiProvider, request_body_that_differs_from_its_reference_is_refused_without_io) {
    prompt_by_reference = true;
    change_reference = [](ayni_body_ref_v1 &ref) { ref.digest ^= 1U; };
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.effect, AYNI_CALL_EFFECT_NONE);
    EXPECT_EQ(server->requests(), 0U);
}
TEST_F(OpenaiProvider, service_token_usage_reaches_task_and_owner_views) {
    start(
        R"({"usage":{"prompt_tokens":1234,"completion_tokens":56,"total_tokens":1290},"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"done"}}]})");
    create();
    wait_done();
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    const auto &usage = seen_result.model_usage;
    EXPECT_EQ(usage.kind, AYNI_MODEL_USAGE_REPORTED);
    EXPECT_EQ(usage.input_tokens, 1234U);
    EXPECT_EQ(usage.output_tokens, 56U);
    EXPECT_EQ(usage.total_tokens, 1290U);
    ayni_call_info_v1 info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_call(work.kernel(), seen_result.logical_call_id, &info), AYNI_OK);
    EXPECT_EQ(info.model_usage.kind, usage.kind);
    EXPECT_EQ(info.model_usage.total_tokens, usage.total_tokens);
}
TEST_F(OpenaiProvider, omitted_usage_is_explicitly_unavailable) {
    start(text_reply);
    create();
    wait_done();
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(seen_result.model_usage.kind, AYNI_MODEL_USAGE_NONE);
    EXPECT_EQ(seen_result.model_usage.total_tokens, 0U);
}
TEST_F(OpenaiProvider, invalid_optional_statistics_preserve_known_model_output) {
    start(
        R"({"usage":{"prompt_tokens":50,"completion_tokens":10,"total_tokens":59},"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"done"}}]})");
    create();
    wait_done();
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(seen_result.effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_EQ(reply, "done");
    EXPECT_EQ(seen_result.model_usage.kind, AYNI_MODEL_USAGE_INVALID);
    EXPECT_EQ(seen_result.model_usage.total_tokens, 0U);
    EXPECT_EQ(server->requests(), 1U);
}
TEST_F(OpenaiProvider, ambiguous_optional_usage_does_not_erase_the_completion) {
    start(
        R"({"usage":{"prompt_tokens":1,"prompt_tokens":2,"completion_tokens":3,"total_tokens":5},"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"done"}}]})");
    create();
    wait_done();
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(reply, "done");
    EXPECT_EQ(seen_result.effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_EQ(seen_result.model_usage.kind, AYNI_MODEL_USAGE_INVALID);
    EXPECT_EQ(server->requests(), 1U);
}
// ADR-0074: framed instructions become the system message, and the data alone the user message.
TEST_F(OpenaiProvider, framed_instructions_become_the_system_message) {
    prompt = AYNI_MODEL_INSTRUCTIONS_PREFIX "12\nrules 世界hello";
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_NE(
        server->request().find(
            R"("messages":[{"role":"system","content":"rules 世界"},{"role":"user","content":"hello"}])"),
        std::string::npos);
}
TEST_F(OpenaiProvider, empty_framed_instructions_send_only_the_user_message) {
    prompt = AYNI_MODEL_INSTRUCTIONS_PREFIX "0\nhello";
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_NE(server->request().find(R"("messages":[{"role":"user","content":"hello"}])"),
              std::string::npos);
}
TEST_F(OpenaiProvider, prefix_after_the_start_is_data) {
    prompt = "say " AYNI_MODEL_INSTRUCTIONS_PREFIX "2\nhi";
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_NE(server->request().find(
                  R"("messages":[{"role":"user","content":"say )" AYNI_MODEL_INSTRUCTIONS_PREFIX
                  R"(2\u000ahi"}])"),
              std::string::npos);
}
// A malformed frame fails before anything leaves the device.
struct Frame {
    const char *name;
    const char *body;
};
void PrintTo(const Frame &frame, std::ostream *out) {
    *out << frame.name;
}
class OpenaiFrame : public OpenaiProvider, public ::testing::WithParamInterface<Frame> {};
TEST_P(OpenaiFrame, malformed_instructions_frame_fails_without_egress) {
    prompt = GetParam().body;
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(seen_result.effect, AYNI_CALL_EFFECT_NONE);
    EXPECT_EQ(server->requests(), 0U);
}
INSTANTIATE_TEST_SUITE_P(
    Frames, OpenaiFrame,
    ::testing::Values(Frame{"prefix_only", AYNI_MODEL_INSTRUCTIONS_PREFIX},
                      Frame{"not_a_number", AYNI_MODEL_INSTRUCTIONS_PREFIX "x\nhello"},
                      Frame{"plus_sign", AYNI_MODEL_INSTRUCTIONS_PREFIX "+3\nabcd"},
                      Frame{"leading_zero", AYNI_MODEL_INSTRUCTIONS_PREFIX "03\nabcd"},
                      Frame{"no_newline", AYNI_MODEL_INSTRUCTIONS_PREFIX "3"},
                      Frame{"space_before_newline", AYNI_MODEL_INSTRUCTIONS_PREFIX "3 \nabc"},
                      Frame{"count_past_the_body", AYNI_MODEL_INSTRUCTIONS_PREFIX "9\nabc"},
                      Frame{"count_overflow", AYNI_MODEL_INSTRUCTIONS_PREFIX "4294967296\nabc"},
                      Frame{"split_inside_a_character", AYNI_MODEL_INSTRUCTIONS_PREFIX "1\n世界"}),
    [](const auto &test) { return std::string(test.param.name); });
TEST_F(OpenaiProvider, offered_function_becomes_exact_typed_values) {
    start(tool_reply(R"({"seconds":7,"voice":"calm","text":"hello"})"));
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(seen_result.tool_call_count, 1U);
    EXPECT_EQ(proposal, "show|hello|7second|calm");
    EXPECT_TRUE(reply.empty());
}
// C-USAGE-004: the described mock device proposal executes only after policy
// requests the exact Binding, through normal Core authority/resources/Call.
TEST_F(OpenaiProvider, described_mock_device_runs_through_the_normal_managed_call) {
    execute_proposal = true;
    start(tool_reply(R"({"seconds":7,"voice":"calm","text":"hello"})"));
    create();
    wait_done();
    EXPECT_EQ(device_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(device_result.effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_EQ(device_reply, "hello");
    EXPECT_EQ(presenter.executions(), 1U);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_EQ(usage_lookups, 1U);
    EXPECT_EQ(proposal, "show|hello|7second|calm");
}

// ADR-0082/AP-01: no explanatory fallback may expose an unusable or wrong tool.
TEST_F(OpenaiProvider, fixed_functions_need_no_catalog_usage_at_submit) {
    usage_available = false;
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(seen_result.effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_EQ(usage_lookups, 0U);
}
TEST_F(OpenaiProvider, text_only_requests_need_no_usage_port) {
    usage_available = false;
    model_result_format = AYNI_CALL_RESULT_TEXT;
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_EQ(server->request().find("\"tools\":"), std::string::npos);
    EXPECT_EQ(usage_lookups, 0U);
}
// C-USAGE-001: absence depends on the actual tool view, not the result format.
TEST_F(OpenaiProvider, empty_business_policy_still_uses_the_fixed_native_functions) {
    usage_available = false;
    offer_tools = false;
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_NE(server->request().find("\"tools\":"), std::string::npos);
    EXPECT_EQ(usage_lookups, 0U);
}
TEST_F(OpenaiProvider, missing_candidate_usage_is_not_adopted_or_sent_to_model) {
    usage_status = AYNI_NOT_FOUND;
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(seen_result.effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_EQ(usage_lookups, 1U);
    EXPECT_EQ(adoption_status, AYNI_NOT_FOUND);
}

struct InvalidUsage {
    const char *name;
    void (*change)(ayni_tool_usage_v1 &);
};
class OpenaiUsage : public OpenaiProvider, public ::testing::WithParamInterface<InvalidUsage> {};
TEST_P(OpenaiUsage, invalid_description_is_rejected_during_trusted_adoption) {
    change_usage = GetParam().change;
    start(text_reply);
    EXPECT_NE(adoption_status, AYNI_OK);
    EXPECT_EQ(usage_lookups, 1U);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(server->requests(), 1U); // Fixed function metadata needs no catalog description.
    EXPECT_EQ(usage_lookups, 1U);      // No worker/submit lookup.
}
INSTANTIATE_TEST_SUITE_P(
    Usage, OpenaiUsage,
    ::testing::Values(
        InvalidUsage{"old_layout", [](auto &v) { --v.struct_size; }},
        InvalidUsage{"future_layout", [](auto &v) { ++v.struct_size; }},
        InvalidUsage{"binding_id", [](auto &v) { v.binding_id = text("another"); }},
        InvalidUsage{"binding_version", [](auto &v) { ++v.binding_version; }},
        InvalidUsage{"capability_id", [](auto &v) { v.capability_id = text("another"); }},
        InvalidUsage{"capability_version", [](auto &v) { ++v.capability_version; }},
        InvalidUsage{"reserved", [](auto &v) { v.reserved = 1; }},
        InvalidUsage{"purpose_missing", [](auto &v) { v.purpose = {}; }},
        InvalidUsage{"null_text", [](auto &v) { v.text_usage.data = nullptr; }},
        InvalidUsage{"result_missing", [](auto &v) { v.result_usage = {}; }},
        InvalidUsage{"examples_missing", [](auto &v) { v.examples = {}; }},
        InvalidUsage{"invalid_utf8", [](auto &v) { v.purpose = {"\xc0\x80", 2}; }},
        InvalidUsage{"tool_budget", [](auto &v) { v.purpose.size = AYNI_TOOL_USAGE_MAX_BYTES + 1; }},
        InvalidUsage{"missing_field", [](auto &v) { --v.field_count; }},
        InvalidUsage{"extra_field", [](auto &v) { ++v.field_count; }},
        InvalidUsage{"null_fields", [](auto &v) { v.fields = nullptr; }},
        InvalidUsage{"field_layout", [](auto &v) {
            ++const_cast<ayni_tool_field_usage_v1 *>(v.fields)[0].struct_size;
        }},
        InvalidUsage{"unknown_field", [](auto &v) {
            const_cast<ayni_tool_field_usage_v1 *>(v.fields)[0].field_id = 99;
        }},
        InvalidUsage{"duplicate_field", [](auto &v) {
            const_cast<ayni_tool_field_usage_v1 *>(v.fields)[0].field_id = v.fields[1].field_id;
        }},
        InvalidUsage{"empty_field_explanation", [](auto &v) {
            const_cast<ayni_tool_field_usage_v1 *>(v.fields)[0].description = {};
        }},
        InvalidUsage{"field_budget", [](auto &v) {
            const_cast<ayni_tool_field_usage_v1 *>(v.fields)[0].description.size =
                AYNI_TOOL_FIELD_USAGE_MAX_BYTES + 1;
        }}),
    [](const auto &v) { return std::string(v.param.name); });

// Both limits are inclusive and count UTF-8 bytes. Order is by exact field ID.
TEST_F(OpenaiProvider, exact_tool_and_field_description_budgets_are_copied_during_adoption) {
    std::string field(AYNI_TOOL_FIELD_USAGE_MAX_BYTES, 'f');
    change_usage = [&](auto &usage) {
        const_cast<ayni_tool_field_usage_v1 *>(usage.fields)[0].description = {
            field.data(), static_cast<uint32_t>(field.size())};
        uint32_t other = usage.binding_id.size + usage.capability_id.size + usage.text_usage.size +
                         usage.result_usage.size + usage.examples.size;
        for (uint32_t at = 0; at < usage.field_count; ++at) {
            other += usage.fields[at].description.size;
        }
        usage_purpose.assign(AYNI_TOOL_USAGE_MAX_BYTES - other, 'p');
        usage.purpose = {usage_purpose.data(), static_cast<uint32_t>(usage_purpose.size())};
    };
    start(text_reply);
    ASSERT_EQ(adoption_status, AYNI_OK);
    const auto goal = create();
    wait_request();
    ayni_capability_detail_v1 detail{};
    detail.struct_size = sizeof(detail);
    ASSERT_EQ(ayni_inspect_task_capability(work.kernel(), work.goal(goal).last_task_id,
                                           text("show"), 1, &detail),
              AYNI_OK);
    EXPECT_EQ(string_of(detail.usage.purpose), usage_purpose);
    EXPECT_EQ(detail.usage.fields[1].field_id, 2U);
    EXPECT_EQ(string_of(detail.usage.fields[1].description), field);
    std::fill(field.begin(), field.end(), 'x');
    EXPECT_EQ(string_of(detail.usage.fields[1].description),
              std::string(AYNI_TOOL_FIELD_USAGE_MAX_BYTES, 'f'));
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(server->request().find(usage_purpose), std::string::npos);
}

// Model usage uses the same pure scalar validator as existing payload owners.
TEST(ToolUsage, unicode_validation_is_public_pure_and_length_delimited) {
    EXPECT_EQ(ayni_validate_utf8({nullptr, 0}), AYNI_OK);
    EXPECT_EQ(ayni_validate_utf8({nullptr, 1}), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_validate_utf8({"\0\xe4\xb8\x96", 4}), AYNI_OK);
    EXPECT_EQ(ayni_validate_utf8({"\xed\xa0\x80", 3}), AYNI_INVALID_ARGUMENT);
}

TEST_F(OpenaiProvider, individually_bounded_strings_cannot_exceed_the_total_usage_budget) {
    usage_purpose.assign(3900, 'p');
    start(text_reply);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(seen_result.effect, AYNI_CALL_EFFECT_COMPLETE);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_NE(adoption_status, AYNI_OK);
}

// ADR-0082/AP-01: neither the I/O codec nor later result decoding can borrow
// extension buffers after submit. A subsequent lookup is allowed to reuse them.
TEST(ToolUsage, copied_usage_survives_source_reuse_and_encodes_the_original_description) {
    using namespace ayni::providers::openai_compat;
    ayni_tool_v1 view{};
    view.struct_size = sizeof(view);
    view.binding_id = text("display");
    view.binding_version = 1;
    view.capability_id = text("mock.display");
    view.capability_version = 1;
    view.text_limit = 64;
    ayni_authority_value_set_v1 duration{};
    duration.struct_size = sizeof(duration);
    duration.kind = AYNI_AUTHORITY_I64;
    duration.unit = text("second");
    duration.values.integer = {1, 2};
    view.field_count = 1;
    view.fields[0] = {sizeof(ayni_authority_field_desc_v1), 1, AYNI_AUTHORITY_PARAMETER, 0,
                      duration};
    std::array<ayni_schema_node_v1, 3> schema{};
    schema[0].kind = AYNI_VALUE_OBJECT;
    schema[0].subtree_end = 3;
    schema[1].kind = AYNI_VALUE_I64;
    schema[1].field_id = 17;
    schema[1].subtree_end = 2;
    schema[1].flags = AYNI_SCHEMA_REQUIRED;
    schema[1].name = text("seconds");
    schema[1].unit = text("second");
    schema[1].min_i64 = 1;
    schema[1].max_i64 = 10;
    schema[2].kind = AYNI_VALUE_TEXT;
    schema[2].field_id = 2;
    schema[2].subtree_end = 3;
    schema[2].flags = AYNI_SCHEMA_REQUIRED;
    schema[2].name = text("text");
    schema[2].max_count = 64;
    ayni_schema_node_v1 result{};
    result.kind = AYNI_VALUE_TEXT;
    result.subtree_end = 1;
    result.max_count = 64;
    ayni_authority_projection_v1 projection{};
    projection.field_id = 1;
    projection.path_count = 1;
    projection.path[0] = 17;
    projection.unit = text("second");
    ayni_business_contract_v1 definition{{schema.data(), 3}, {&result, 1}, 1, 0, &projection};
    std::array<uint8_t, 256> domain{};
    uint32_t domain_size = 0;
    auto original_field = view.fields[0];
    original_field.domain.values.integer.max_inclusive = 10;
    ASSERT_EQ(
        ayni::catalog::encode_domain(&original_field, 1, domain.data(), domain.size(), domain_size),
        AYNI_OK);
    ayni::catalog::CallContract execution{};
    execution.form = ayni::catalog::CallForm::unary;
    execution.category = ayni::catalog::CallClass::tool;
    execution.idempotency = ayni::catalog::CallIdempotency::idempotent;
    execution.query = ayni::catalog::QueryAuthority::none;
    execution.cancel = ayni::catalog::CancelMode::none;
    execution.timeout_ms = 1000;
    execution.input_format = AYNI_CALL_INPUT_VALUE;
    execution.result_format = AYNI_CALL_RESULT_TEXT;
    execution.input_text_limit = 256;
    execution.result_text_limit = 64;
    std::array<uint8_t, 4096> proof{};
    uint32_t proof_size = 0;
    ASSERT_EQ(ayni::catalog::encode_business(::ayni::test::codec_workspace(),
                                             {domain.data(), domain_size}, execution, definition,
                                             proof.data(), proof.size(), proof_size),
              AYNI_OK);
    view.original_domain = {domain.data(), domain_size};
    view.business_contract = {proof.data(), proof_size};
    ayni::detail::ToolUsage copied;
    std::string purpose = "Original mock display usage";
    std::string field_text = "Original display duration explanation";
    ayni_tool_field_usage_v1 field{
        sizeof(field), 1, {field_text.data(), static_cast<uint32_t>(field_text.size())}};
    ayni_tool_usage_v1 usage{sizeof(usage),
                             1,
                             view.binding_id,
                             1,
                             view.capability_id,
                             {purpose.data(), static_cast<uint32_t>(purpose.size())},
                             text("Display the supplied text."),
                             text("Simulated result only."),
                             text("hello"),
                             1,
                             0,
                             &field};
    ASSERT_TRUE(copied.assign(usage, view));
    std::fill(purpose.begin(), purpose.end(), 'x');
    std::fill(field_text.begin(), field_text.end(), 'x');
    field.field_id = 99;
    usage.text_usage = text("Changed source view");
    ayni_tool_usage_v1 retained{};
    std::array<ayni_tool_field_usage_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> fields{};
    copied.read(retained, fields);
    EXPECT_EQ(string_of(retained.purpose), "Original mock display usage");
    EXPECT_EQ(string_of(retained.text_usage), "Display the supplied text.");
    EXPECT_EQ(string_of(retained.fields[0].description), "Original display duration explanation");
    EXPECT_EQ(retained.fields[0].field_id, 1U);
    std::array<char, 4096> buffer{};
    ayni::json::Writer writer(buffer.data(), buffer.size());
    ayni::json::write_schema(writer, definition.input);
    size_t size = 0;
    ASSERT_TRUE(writer.finish(size));
    const std::string encoded(buffer.data(), size);
    EXPECT_NE(encoded.find("\"seconds\""), std::string::npos);
    EXPECT_NE(encoded.find("\"minimum\":1,\"maximum\":10"), std::string::npos);
}
// ADR-0084: machine bounds survive the JSON adapter with exact integer/decimal
// descriptions and binary64 roundtrip bounds; byte limits are not Unicode lengths.
TEST(OpenaiSchema, numeric_and_text_bounds_are_exposed_without_precision_loss) {
    using namespace ayni::providers::openai_compat;
    std::array<ayni_schema_node_v1, 6> nodes{};
    nodes[0].kind = AYNI_VALUE_OBJECT;
    nodes[0].subtree_end = static_cast<uint32_t>(nodes.size());
    const std::array<ayni_string_v1, 5> names{text("ratio"), text("signed"), text("unsigned"),
                                              text("price"), text("label")};
    for (uint32_t at = 1; at < nodes.size(); ++at) {
        nodes[at].field_id = at;
        nodes[at].subtree_end = at + 1;
        nodes[at].name = names[at - 1];
        nodes[at].flags = AYNI_SCHEMA_REQUIRED;
    }
    nodes[1].kind = AYNI_VALUE_FLOAT64;
    nodes[1].min_float64 = 0.125;
    nodes[1].max_float64 = 0.875;
    nodes[2].kind = AYNI_VALUE_I64;
    nodes[2].min_i64 = INT64_MIN;
    nodes[2].max_i64 = INT64_MAX;
    nodes[3].kind = AYNI_VALUE_U64;
    nodes[3].max_u64 = UINT64_MAX;
    nodes[4].kind = AYNI_VALUE_DECIMAL;
    nodes[4].min_i64 = 100;
    nodes[4].max_i64 = 200;
    nodes[4].scale = 2;
    nodes[4].unit = text("dollar");
    nodes[5].kind = AYNI_VALUE_TEXT;
    nodes[5].min_count = 3;
    nodes[5].max_count = 9;
    ayni_schema_node_v1 result{};
    result.kind = AYNI_VALUE_TEXT;
    result.subtree_end = 1;
    result.max_count = 16;
    const ayni_business_contract_v1 business{
        {nodes.data(), static_cast<uint32_t>(nodes.size())}, {&result, 1}, 0, 0, nullptr};
    const std::array<uint8_t, 4> domain{{1, 0, 0, 0}};
    ayni::catalog::CallContract execution{};
    execution.form = ayni::catalog::CallForm::unary;
    execution.category = ayni::catalog::CallClass::tool;
    execution.idempotency = ayni::catalog::CallIdempotency::idempotent;
    execution.query = ayni::catalog::QueryAuthority::none;
    execution.cancel = ayni::catalog::CancelMode::none;
    execution.timeout_ms = 1000;
    execution.input_format = AYNI_CALL_INPUT_VALUE;
    execution.result_format = AYNI_CALL_RESULT_TEXT;
    execution.input_text_limit = 512;
    execution.result_text_limit = 16;
    std::array<uint8_t, 4096> proof{};
    uint32_t size = 0;
    ASSERT_EQ(ayni::catalog::encode_business(::ayni::test::codec_workspace(),
                                             {domain.data(), domain.size()}, execution, business,
                                             proof.data(), proof.size(), size),
              AYNI_OK);
    ayni_tool_v1 view{};
    view.struct_size = sizeof(view);
    view.binding_version = view.capability_version = 1;
    view.binding_id = view.capability_id = text("typed.bounds");
    view.text_limit = 512;
    view.original_domain = {domain.data(), domain.size()};
    view.business_contract = {proof.data(), size};
    std::array<char, 4096> buffer{};
    ayni::json::Writer writer(buffer.data(), buffer.size());
    ayni::json::write_schema(writer, business.input);
    size_t written = 0;
    ASSERT_TRUE(writer.finish(written));
    const std::string encoded(buffer.data(), written);
    EXPECT_NE(encoded.find("\"minimum\":0.125,\"maximum\":0.875"), std::string::npos);
    EXPECT_NE(encoded.find("[-9223372036854775808, 9223372036854775807]"), std::string::npos);
    EXPECT_NE(encoded.find("[0, 18446744073709551615]"), std::string::npos);
    EXPECT_NE(encoded.find("Scale 2. Exact coefficient range [100, 200]"), std::string::npos);
    EXPECT_NE(encoded.find("UTF-8 maximum 9 bytes; minimum 3 UTF-8 bytes"), std::string::npos);
}

TEST_F(OpenaiProvider, malformed_json_is_model_output_failure) {
    start("{broken");
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
}
// The body limit follows max_output_tokens (ADR-0095): 64 KiB of metadata plus 96 B per token.
TEST_F(OpenaiProvider, oversized_http_body_is_bounded_model_output_failure) {
    start(std::string(65536 + 96 + 1, 'x'), 200, 0, 1);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
}
TEST_F(OpenaiProvider, timeout_returns_borrow_without_retrying_unknown_execution) {
    start(text_reply, 200, 1000, 1024, 100);
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
TEST_P(OpenaiArguments,
       invalid_arguments_return_normal_tool_refusals_after_known_model_completion) {
    start(tool_reply(GetParam()));
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(seen_result.status, AYNI_OK);
    EXPECT_EQ(seen_result.tool_call_count, 1U);
    EXPECT_NE(proposal_error, AYNI_OK);
    EXPECT_EQ(presenter.executions(), 0U);
    EXPECT_EQ(work.task(work.goal(last_goal).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
}
INSTANTIATE_TEST_SUITE_P(
    Arguments, OpenaiArguments,
    ::testing::Values(R"({"seconds":11,"voice":"calm","text":"hi"})",
                      R"({"seconds":7.5,"voice":"calm","text":"hi"})",
                      R"({"seconds":18446744073709551615,"voice":"calm","text":"hi"})",
                      R"({"seconds":7,"voice":"other","text":"hi"})",
                      R"({"seconds":7,"text":"hi"})",
                      R"({"seconds":7,"seconds":8,"voice":"calm","text":"hi"})",
                      R"({"seconds":7,"voice":"calm","text":"hi","extra":true})",
                      R"({"seconds":7,"voice":"calm","text":null})", "{}", "broken"));
TEST_F(OpenaiProvider, unknown_function_is_a_normal_refusal_after_known_model_completion) {
    start(tool_reply(R"({"seconds":7,"voice":"calm","text":"hi"})", "unknown"));
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(proposal_error, AYNI_NOT_FOUND);
    EXPECT_EQ(presenter.executions(), 0U);
}
TEST_F(OpenaiProvider, body_over_tool_limit_is_invalid_argument) {
    start(
        tool_reply("{\"seconds\":7,\"voice\":\"calm\",\"text\":\"" + std::string(65, 'x') + "\"}"));
    create();
    wait_done();
    const auto task = work.task(work.goal(last_goal).last_task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_NE(proposal_error, AYNI_OK);
    EXPECT_EQ(presenter.executions(), 0U);
}
TEST_F(OpenaiProvider, redirect_does_not_send_credentials_to_another_server) {
    LoopbackHttp redirected(text_reply);
    start("", 302, 0, 1024, 1500, "Location: " + redirected.url() + "/chat/completions\r\n");
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(redirected.requests(), 0U);
}
// C-CALL-014: the list cardinality is the only limit on a model answer; past it the answer
// fails with a FAILED receipt.
class OpenaiListLimit : public OpenaiProvider, public ::testing::WithParamInterface<unsigned> {};
TEST_P(OpenaiListLimit, list_limits_return_capacity_and_release_the_borrow) {
    const auto one = tool_reply(R"({"seconds":7,"voice":"calm","text":"hi"})");
    const auto begin = one.find("[{\"id\"") + 1;
    const auto end = one.rfind("]}}]}");
    const auto call = one.substr(begin, end - begin);
    std::string response = one.substr(0, begin);
    for (unsigned i = 0; i < GetParam(); ++i) {
        if (i) {
            response += ',';
        }
        auto next = call;
        const auto identity = next.find("call_1");
        next.replace(identity, sizeof("call_1") - 1, "call_" + std::to_string(i));
        response += next;
    }
    response += one.substr(end);
    start(response);
    create();
    wait_done();
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(seen_result.status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(seen_result.reason, AYNI_REASON_MODEL_OUTPUT);
    EXPECT_EQ(provider.pending(), 0U);
}
INSTANTIATE_TEST_SUITE_P(Limits, OpenaiListLimit, ::testing::Values(9U, 65U));
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
    model_capacity = 2; // Exercise the Provider bound separately from Core model-slot admission.
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
// ADR-0085: transport functions stay fixed independently of catalog size/input.
TEST(OpenaiConfiguration, native_functions_have_one_stable_transport_definition) {
    using namespace ayni::providers::openai_compat;
    const Protocol codec;
    Config config;
    config.model = "fixture";
    std::string first, second;
    ASSERT_EQ(codec.encode(config, text("first input"), first), AYNI_OK);
    ASSERT_EQ(codec.encode(config, text("second input"), second), AYNI_OK);
    const auto start = first.find("\"tools\":");
    ASSERT_NE(start, std::string::npos);
    EXPECT_EQ(first.substr(start), second.substr(second.find("\"tools\":")));
    for (auto name : {"ayni_list_capabilities", "ayni_inspect_capability", "ayni_call_capability",
                      "ayni_shell"}) {
        EXPECT_NE(first.find(name), std::string::npos);
    }
    EXPECT_EQ(first.find("tool_0"), std::string::npos);
}
TEST(OpenaiConfiguration, rejects_unsafe_urls_and_missing_key_without_starting_io) {
    ayni::runtime::MemoryBodies bodies;
    Config config;
    config.bodies = bodies.store();
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

#include "model_wire_fixture.h"
namespace {
using WireApi = ayni::providers::model_protocol::Api;
class OpenaiWireMatrix : public ayni::test::OpenaiFixture,
                         public ::testing::WithParamInterface<std::tuple<WireApi, bool, bool>> {};
TEST_P(OpenaiWireMatrix, NativeHttpPublishesOneStoredCompleteReply) {
    // ADR-0098: the real curl worker consumes the shared codec for every API/mode.
    const auto [api, streaming, tools] = GetParam();
    std::string preview;
    std::thread::id preview_thread;
    configure_protocol = [&](Config &config) {
        config.api = api;
        config.stream = streaming;
        config.reply_encoding = ayni::model::ReplyEncoding::framed;
        config.preview = {&preview, [](void *context, std::string_view value) {
                              static_cast<std::string *>(context)->append(value);
                          }};
        if (streaming) {
            config.preview = {&preview_thread, [](void *context, std::string_view) {
                                  *static_cast<std::thread::id *>(context) =
                                      std::this_thread::get_id();
                              }};
        }
    };
    const auto body =
        streaming                   ? ayni::test::model_wire::stream_answer(api, tools)
        : api == WireApi::responses ? ayni::test::model_wire::responses_answer(tools)
        : tools
            ? R"({"choices":[{"finish_reason":"tool_calls","message":{"role":"assistant","content":"你好","tool_calls":[{"id":"call_1","type":"function","function":{"name":"ayni_call_capability","arguments":"{}"}}]}}],"usage":{"prompt_tokens":30,"completion_tokens":4,"total_tokens":34}})"
            : R"({"choices":[{"finish_reason":"stop","message":{"role":"assistant","content":"你好"}}],"usage":{"prompt_tokens":30,"completion_tokens":4,"total_tokens":34}})";
    start(body);
    create();
    wait_done();
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_NE(seen_result.body.id, 0U);
    EXPECT_EQ(seen_result.tool_call_count, tools ? 1U : 0U);
    EXPECT_EQ(seen_result.model_usage.input_tokens, 30U);
    ayni_string_v1 text_value{};
    ayni::model::Continuation state;
    ASSERT_EQ(ayni::model::read_reply_frame({reply.data(), uint32_t(reply.size())},
                                            ayni::model::ReplyEncoding::framed, text_value, state),
              AYNI_OK);
    EXPECT_EQ(std::string_view(text_value.data, text_value.size), "你好");
    EXPECT_NE(state.codec.size, 0U);
    EXPECT_EQ(server->requests(), 1U);
    EXPECT_NE(server->request().find(api == WireApi::responses ? "POST /responses HTTP/1.1"
                                                               : "POST /chat/completions HTTP/1.1"),
              std::string::npos);
    if (streaming) {
        EXPECT_NE(preview_thread, owner_thread);
        EXPECT_NE(preview_thread, std::thread::id{});
    }
}
INSTANTIATE_TEST_SUITE_P(LocalHttp, OpenaiWireMatrix,
                         ::testing::Combine(::testing::Values(WireApi::chat_completions,
                                                              WireApi::responses),
                                            ::testing::Bool(), ::testing::Bool()));
} // namespace

namespace {
TEST_F(OpenaiProvider, StreamingHttpErrorKeepsClassificationWithoutPublishingVendorData) {
    // C-PROV-004/ADR-0098: ordinary HTTP JSON errors are not SSE and have no preview.
    configure_protocol = [](Config &config) { config.stream = true; };
    start(R"({"error":{"message":"fixture-vendor-detail"}})", 401);
    create();
    wait_done();
    EXPECT_EQ(seen_result.status, AYNI_DENIED);
    EXPECT_EQ(seen_result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(reply.find("fixture-vendor-detail"), std::string::npos);
    EXPECT_EQ(server->requests(), 1U);
}
} // namespace
