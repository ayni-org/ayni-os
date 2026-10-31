#include "workspace_fixture.h"
// White-box canonical payload tests, C-CALL-001/C-AUTH-017 and ADR-0045.
// These exercise bounded business data, not actual persistence or model APIs.
#include "authority/scope/scope.h"
#include "catalog/capability/business_encoding.h"
#include "catalog/capability/field_domain.h"
#include "foundation/little_endian.h"
#include "work/call/call_input.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

namespace {
using namespace ayni::work;
constexpr uint32_t capacity = 512;
constexpr uint32_t text_limit = 64;

class CallInputTest : public ::testing::Test {
  protected:
    std::array<uint8_t, capacity> domain_storage{};
    std::array<uint8_t, capacity> body{};
    uint32_t domain_size = 0;
    uint32_t size = 0;
    std::array<ayni_value_node_v1, 3> nodes{};
    ayni_value_v1 input{nodes.data(), 3};
    std::array<ayni_schema_node_v1, 3> schema{};
    ayni_schema_node_v1 result{};
    ayni_authority_projection_v1 path{};
    ayni::catalog::CallContract contract{};
    std::array<uint8_t, 4096> proof{};
    ayni_business_contract_v1 business{};
    void SetUp() override {
        ayni_authority_value_set_v1 values{};
        values.struct_size = sizeof(values);
        values.kind = AYNI_AUTHORITY_I64;
        values.unit = {"count", 5};
        values.values.integer = {0, 10};
        const ayni_authority_field_desc_v1 field{sizeof(ayni_authority_field_desc_v1), 1,
                                                 AYNI_AUTHORITY_PARAMETER, 0, values};
        ASSERT_EQ(
            ayni::catalog::encode_domain(&field, 1, domain_storage.data(), capacity, domain_size),
            AYNI_OK);
        nodes[0].kind = schema[0].kind = AYNI_VALUE_OBJECT;
        nodes[0].subtree_end = schema[0].subtree_end = 3;
        nodes[1].kind = schema[1].kind = AYNI_VALUE_I64;
        nodes[1].field_id = schema[1].field_id = 1;
        nodes[1].subtree_end = schema[1].subtree_end = 2;
        nodes[1].value.integer = 3;
        schema[1].flags = AYNI_SCHEMA_REQUIRED;
        schema[1].name = {"count", 5};
        schema[1].unit = {"count", 5};
        schema[1].max_i64 = 10;
        nodes[2].kind = schema[2].kind = AYNI_VALUE_TEXT;
        nodes[2].field_id = schema[2].field_id = 2;
        nodes[2].subtree_end = schema[2].subtree_end = 3;
        schema[2].flags = AYNI_SCHEMA_REQUIRED;
        schema[2].name = {"body", 4};
        schema[2].max_count = text_limit;
        result.kind = AYNI_VALUE_TEXT;
        result.subtree_end = 1;
        result.max_count = text_limit;
        path.field_id = 1;
        path.path_count = 1;
        path.path[0] = 1;
        path.unit = {"count", 5};
        business = {{schema.data(), 3}, {&result, 1}, 1, 0, &path};
        contract.form = ayni::catalog::CallForm::unary;
        contract.category = ayni::catalog::CallClass::tool;
        contract.idempotency = ayni::catalog::CallIdempotency::idempotent;
        contract.query = ayni::catalog::QueryAuthority::none;
        contract.cancel = ayni::catalog::CancelMode::none;
        contract.timeout_ms = 1000;
        contract.input_format = AYNI_CALL_INPUT_VALUE;
        contract.result_format = AYNI_CALL_RESULT_TEXT;
        contract.input_text_limit = 128;
        contract.result_text_limit = text_limit;
        compile_contract();
    }
    ayni_bytes_v1 domain() const { return {domain_storage.data(), domain_size}; }
    void compile_contract() {
        uint32_t bytes = 0;
        ASSERT_EQ(ayni::catalog::encode_business(::ayni::test::codec_workspace(), domain(),
                                                 contract, business, proof.data(),
                                                 static_cast<uint32_t>(proof.size()), bytes),
                  AYNI_OK);
        contract.business_contract = {proof.data(), bytes};
    }
    ayni_status_t encode() {
        return encode_business_input(::ayni::test::codec_workspace(), domain(), contract, input,
                                     body.data(), capacity, size);
    }
    ayni_status_t read(CallInputView &out) {
        return read_call_input(::ayni::test::codec_workspace(), domain(), {body.data(), size},
                               contract, out);
    }
    static ayni_string_v1 body_text(const CallInputView &view) {
        return view.value.nodes[2].value.text;
    }
};

TEST_F(CallInputTest, copies_text_and_concrete_arguments_without_retaining_input_views) {
    std::string text = "你好，Ayni";
    nodes[2].value.text = {text.data(), static_cast<uint32_t>(text.size())};
    ASSERT_EQ(encode(), AYNI_OK);
    const auto expected = text;
    text.assign(text.size(), 'x');
    nodes[1].value.integer = 9;
    CallInputView view{};
    ASSERT_EQ(read(view), AYNI_OK);
    EXPECT_EQ(std::string(body_text(view).data, body_text(view).size), expected);
    ayni::catalog::DomainReader fields(view.arguments);
    ayni::catalog::DomainField value{};
    ASSERT_TRUE(fields.next(value));
    EXPECT_EQ(value.values.minimum, 3);
    EXPECT_EQ(value.values.maximum, 3);
    EXPECT_TRUE(fields.ok());
    const auto authorization =
        ayni::authority::contains_request(domain(), domain(), view.arguments);
    EXPECT_EQ(authorization.status, AYNI_OK);
    EXPECT_TRUE(authorization.covered);
}

TEST_F(CallInputTest, empty_text_and_embedded_zero_are_length_delimited) {
    ASSERT_EQ(encode(), AYNI_OK);
    CallInputView view{};
    ASSERT_EQ(read(view), AYNI_OK);
    EXPECT_EQ(body_text(view).size, 0U);
    constexpr char text[] = {'a', '\0', 'b'};
    nodes[2].value.text = {text, sizeof(text)};
    ASSERT_EQ(encode(), AYNI_OK);
    ASSERT_EQ(read(view), AYNI_OK);
    ASSERT_EQ(body_text(view).size, sizeof(text));
    EXPECT_EQ(std::memcmp(body_text(view).data, text, sizeof(text)), 0);
}

TEST_F(CallInputTest, limits_and_small_buffers_never_publish_partial_input) {
    nodes[2].value.text = {"hello", 5};
    uint32_t required = 0;
    ASSERT_EQ(encode_business_input(::ayni::test::codec_workspace(), domain(), contract, input,
                                    nullptr, 0, required),
              AYNI_BUFFER_TOO_SMALL);
    body.fill(0xA5);
    size = 99;
    EXPECT_EQ(encode_business_input(::ayni::test::codec_workspace(), domain(), contract, input,
                                    body.data(), required - 1U, size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, required);
    for (const auto byte : body) {
        EXPECT_EQ(byte, 0xA5U);
    }
    size = 99;
    auto limited = contract;
    limited.input_text_limit = required - 1U;
    EXPECT_EQ(encode_business_input(::ayni::test::codec_workspace(), domain(), limited, input,
                                    body.data(), capacity, size),
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(size, 99U);
    EXPECT_EQ(body[0], 0xA5U);
    ASSERT_EQ(encode(), AYNI_OK);
    CallInputView view{};
    EXPECT_EQ(read_call_input(::ayni::test::codec_workspace(), domain(), {body.data(), size},
                              limited, view),
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(view.value.count, 0U);
}

TEST_F(CallInputTest, text_rejects_invalid_utf8_and_accepts_unicode_scalar_boundaries) {
    const std::string invalid[] = {"\x80",
                                   "\xC0\x80",
                                   "\xC1\xBF",
                                   "\xC2",
                                   "\xE0\x80\x80",
                                   "\xED\xA0\x80",
                                   "\xF0\x80\x80\x80",
                                   "\xF4\x90\x80\x80",
                                   "\xF5\x80\x80\x80",
                                   "\xC2x"};
    for (const auto &text : invalid) {
        nodes[2].value.text = {text.data(), static_cast<uint32_t>(text.size())};
        EXPECT_EQ(encode(), AYNI_INVALID_ARGUMENT);
    }
    const std::string valid[] = {
        "\x7F",         "\xC2\x80",     "\xDF\xBF",         "\xE0\xA0\x80",
        "\xED\x9F\xBF", "\xEE\x80\x80", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"};
    for (const auto &text : valid) {
        nodes[2].value.text = {text.data(), static_cast<uint32_t>(text.size())};
        ASSERT_EQ(encode(), AYNI_OK);
        CallInputView view{};
        ASSERT_EQ(read(view), AYNI_OK);
        EXPECT_EQ(std::string(body_text(view).data, body_text(view).size), text);
    }
}

TEST_F(CallInputTest, missing_or_noninteger_business_fields_are_not_authorization_claims) {
    body.fill(0xA5);
    nodes[1].kind = AYNI_VALUE_BOOL;
    nodes[1].value.boolean = 1;
    EXPECT_EQ(encode(), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(body[0], 0xA5U);
    nodes[1].kind = AYNI_VALUE_I64;
    nodes[1].value.integer = 3;
    nodes[1].field_id = 3;
    EXPECT_EQ(encode(), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(body[0], 0xA5U);
}

TEST_F(CallInputTest, reader_requires_exact_framing_and_preserves_previous_views_on_error) {
    nodes[2].value.text = {"hello", 5};
    ASSERT_EQ(encode(), AYNI_OK);
    CallInputView original{};
    ASSERT_EQ(read(original), AYNI_OK);
    auto output = original;
    EXPECT_EQ(read_call_input(::ayni::test::codec_workspace(), domain(), {body.data(), size - 1U},
                              contract, output),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(body_text(output).data, body_text(original).data);
    EXPECT_EQ(body_text(output).size, body_text(original).size);
    const auto *original_arguments = output.arguments.data;
    constexpr uint32_t root_end_at =
        ayni::catalog::value_encoding_header_bytes + 2 * sizeof(uint32_t);
    ayni::foundation::store_le32(body.data() + root_end_at, UINT32_MAX);
    EXPECT_EQ(read(output), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output.arguments.data, original_arguments);
}
} // namespace
