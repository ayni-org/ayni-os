// White-box canonical payload tests, C-CALL-001/C-AUTH-017 and ADR-0045.
// These exercise bounded business data, not actual persistence or model APIs.
#include "authority/scope/scope.h"
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
    ayni_scope_field_v1 argument{};
    CallInput input{{sizeof(ayni_scope_v1), 1, &argument}, {}};
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
        values.values.integer = {3, 3};
        argument = {sizeof(argument), 1, values};
    }
    ayni_bytes_v1 domain() const { return {domain_storage.data(), domain_size}; }
    ayni_status_t encode() {
        return encode_call_input(domain(), input, text_limit, body.data(), capacity, size);
    }
    ayni_status_t read(CallInputView &out) {
        return read_call_input(domain(), {body.data(), size}, text_limit, out);
    }
};

TEST_F(CallInputTest, copies_text_and_concrete_arguments_without_retaining_input_views) {
    std::string text = "你好，Ayni";
    input.text = {text.data(), static_cast<uint32_t>(text.size())};
    ASSERT_EQ(encode(), AYNI_OK);
    const auto expected = text;
    text.assign(text.size(), 'x');
    argument.allowed.values.integer = {9, 9};
    CallInputView view{};
    ASSERT_EQ(read(view), AYNI_OK);
    EXPECT_EQ(std::string(view.text.data, view.text.size), expected);
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
    ASSERT_EQ(encode_call_input(domain(), input, 0, body.data(), capacity, size), AYNI_OK);
    CallInputView view{};
    ASSERT_EQ(read_call_input(domain(), {body.data(), size}, 0, view), AYNI_OK);
    EXPECT_EQ(view.text.size, 0U);
    constexpr char text[] = {'a', '\0', 'b'};
    input.text = {text, sizeof(text)};
    ASSERT_EQ(encode(), AYNI_OK);
    ASSERT_EQ(read(view), AYNI_OK);
    ASSERT_EQ(view.text.size, sizeof(text));
    EXPECT_EQ(std::memcmp(view.text.data, text, sizeof(text)), 0);
}

TEST_F(CallInputTest, limits_and_small_buffers_never_publish_partial_input) {
    input.text = {"hello", 5};
    uint32_t required = 0;
    ASSERT_EQ(encode_call_input(domain(), input, text_limit, nullptr, 0, required),
              AYNI_BUFFER_TOO_SMALL);
    body.fill(0xA5);
    size = 99;
    EXPECT_EQ(encode_call_input(domain(), input, text_limit, body.data(), required - 1U, size),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(size, required);
    for (const auto byte : body) {
        EXPECT_EQ(byte, 0xA5U);
    }
    size = 99;
    EXPECT_EQ(encode_call_input(domain(), input, 1, body.data(), capacity, size),
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(size, 99U);
    EXPECT_EQ(body[0], 0xA5U);
    ASSERT_EQ(encode(), AYNI_OK);
    CallInputView view{};
    EXPECT_EQ(read_call_input(domain(), {body.data(), size}, 1, view), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(view.text.size, 0U);
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
        input.text = {text.data(), static_cast<uint32_t>(text.size())};
        EXPECT_EQ(encode(), AYNI_INVALID_ARGUMENT);
    }
    const std::string valid[] = {
        "\x7F",         "\xC2\x80",     "\xDF\xBF",         "\xE0\xA0\x80",
        "\xED\x9F\xBF", "\xEE\x80\x80", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"};
    for (const auto &text : valid) {
        input.text = {text.data(), static_cast<uint32_t>(text.size())};
        ASSERT_EQ(encode(), AYNI_OK);
        CallInputView view{};
        ASSERT_EQ(read(view), AYNI_OK);
        EXPECT_EQ(std::string(view.text.data, view.text.size), text);
    }
}

TEST_F(CallInputTest, missing_or_nonsingleton_arguments_are_not_authorization_claims) {
    body.fill(0xA5);
    argument.allowed.values.integer = {2, 4};
    EXPECT_EQ(encode(), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(body[0], 0xA5U);
    input.arguments.field_count = 0;
    EXPECT_EQ(encode(), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(body[0], 0xA5U);
}

TEST_F(CallInputTest, reader_requires_exact_framing_and_preserves_previous_views_on_error) {
    input.text = {"hello", 5};
    ASSERT_EQ(encode(), AYNI_OK);
    CallInputView original{};
    ASSERT_EQ(read(original), AYNI_OK);
    auto output = original;
    EXPECT_EQ(read_call_input(domain(), {body.data(), size - 1U}, text_limit, output),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output.text.data, original.text.data);
    EXPECT_EQ(output.text.size, original.text.size);
    constexpr uint32_t text_length_at = sizeof(uint32_t);
    ayni::foundation::store_le32(body.data() + text_length_at, UINT32_MAX);
    EXPECT_EQ(read(output), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(output.arguments.data, original.arguments.data);
}
} // namespace
