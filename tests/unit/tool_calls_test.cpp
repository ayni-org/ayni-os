// White-box tool-call list codec, ADR-0054 and tool-call list format 1 in
// contracts/storage/records.md. Shape only; offers and authority are Kernel tests.
#include "foundation/little_endian.h"
#include "work/call/tool_calls.h"

#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {
using namespace ayni::work;

ayni_string_v1 text(const char *value) {
    return {value, static_cast<uint32_t>(std::strlen(value))};
}

std::string string_of(ayni_string_v1 view) {
    return view.size == 0U ? std::string() : std::string(view.data, view.size);
}

class ToolCallsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Fields deliberately out of id order; the encoding sorts them.
        token = text("quiet");
        fields[0] = {sizeof(ayni_scope_field_v1), 7, {}};
        fields[0].allowed.struct_size = sizeof(fields[0].allowed);
        fields[0].allowed.kind = AYNI_AUTHORITY_ENUM;
        fields[0].allowed.values.tokens = {&token, 1, 0};
        fields[1] = {sizeof(ayni_scope_field_v1), 2, {}};
        fields[1].allowed.struct_size = sizeof(fields[1].allowed);
        fields[1].allowed.kind = AYNI_AUTHORITY_I64;
        fields[1].allowed.unit = text("minute");
        fields[1].allowed.values.integer = {-5, -5};
        calls[0] = {sizeof(ayni_tool_call_v1),
                    3,
                    text("timer.set"),
                    {sizeof(ayni_scope_v1), 2, fields.data()},
                    text("早点提醒")};
        calls[1] = {sizeof(ayni_tool_call_v1),
                    1,
                    text("lamp.off"),
                    {sizeof(ayni_scope_v1), 0, nullptr},
                    {}};
    }
    ayni_status_t encode(uint32_t count = 2) {
        uint32_t size = 0;
        const auto status = measure_tool_calls(calls.data(), count, size);
        if (status == AYNI_OK) {
            body.assign(size, 0xAAU);
            write_tool_calls(calls.data(), count, body.data(), size);
        }
        return status;
    }
    ayni_bytes_v1 list() const { return {body.data(), static_cast<uint32_t>(body.size())}; }

    ayni_string_v1 token{};
    std::array<ayni_scope_field_v1, 2> fields{};
    std::array<ayni_tool_call_v1, 2> calls{};
    std::vector<uint8_t> body;
};

TEST_F(ToolCallsTest, round_trip_keeps_typed_values_in_canonical_field_order) {
    ASSERT_EQ(encode(), AYNI_OK);
    std::string expected = "早点提醒";
    calls[0].text = text("changed after the post");
    ASSERT_EQ(tool_call_count(list()), 2U);
    ToolCallView call{};
    ASSERT_TRUE(read_tool_call(list(), 0, call));
    EXPECT_EQ(string_of(call.binding_id), "timer.set");
    EXPECT_EQ(call.binding_version, 3U);
    ASSERT_EQ(call.field_count, 2U);
    EXPECT_EQ(call.fields[0].id, 2U);
    EXPECT_EQ(call.fields[0].kind, AYNI_AUTHORITY_I64);
    EXPECT_EQ(string_of(call.fields[0].unit), "minute");
    EXPECT_EQ(call.fields[0].integer, -5);
    EXPECT_EQ(call.fields[1].id, 7U);
    EXPECT_EQ(string_of(call.fields[1].token), "quiet");
    EXPECT_EQ(string_of(call.text), expected);

    std::array<ayni_scope_field_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> scope_fields{};
    std::array<ayni_string_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> tokens{};
    const auto scope = call.scope(scope_fields.data(), tokens.data());
    EXPECT_EQ(scope.struct_size, sizeof(ayni_scope_v1));
    ASSERT_EQ(scope.field_count, 2U);
    EXPECT_EQ(scope.fields[0].allowed.values.integer.min_inclusive, -5);
    EXPECT_EQ(scope.fields[0].allowed.values.integer.max_inclusive, -5);
    EXPECT_EQ(scope.fields[1].allowed.values.tokens.member_count, 1U);
    EXPECT_EQ(string_of(scope.fields[1].allowed.values.tokens.members[0]), "quiet");

    ASSERT_TRUE(read_tool_call(list(), 1, call));
    EXPECT_EQ(string_of(call.binding_id), "lamp.off");
    EXPECT_EQ(call.field_count, 0U);
    EXPECT_FALSE(read_tool_call(list(), 2, call));
    EXPECT_EQ(string_of(call.binding_id), "lamp.off");
}

TEST_F(ToolCallsTest, provider_shape_errors_are_classified_and_measure_nothing) {
    uint32_t size = 9;
    EXPECT_EQ(measure_tool_calls(calls.data(), 0, size), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(measure_tool_calls(nullptr, 1, size), AYNI_INVALID_ARGUMENT);
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX + 1U> many{};
    many.fill(calls[1]);
    EXPECT_EQ(measure_tool_calls(many.data(), AYNI_CALL_TOOL_COUNT_MAX + 1U, size),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(measure_tool_calls(many.data(), AYNI_CALL_TOOL_COUNT_MAX, size), AYNI_OK);
    size = 9;

    auto expect = [&](ayni_status_t status) {
        EXPECT_EQ(measure_tool_calls(calls.data(), 2, size), status);
        EXPECT_EQ(size, 9U);
        SetUp();
    };
    calls[0].struct_size += 8;
    expect(AYNI_UNSUPPORTED_VERSION);
    calls[0].struct_size -= 4;
    expect(AYNI_INVALID_ARGUMENT);
    calls[0].binding_version = 0;
    expect(AYNI_INVALID_ARGUMENT);
    calls[0].binding_id = text("Not.An.Id");
    expect(AYNI_INVALID_ARGUMENT);
    fields[1].field_id = 7; // Duplicate.
    expect(AYNI_INVALID_ARGUMENT);
    fields[1].field_id = 0;
    expect(AYNI_INVALID_ARGUMENT);
    fields[1].allowed.values.integer = {1, 2}; // A range, not a value.
    expect(AYNI_INVALID_ARGUMENT);
    const std::array<ayni_string_v1, 2> two{{text("quiet"), text("loud")}};
    fields[0].allowed.values.tokens = {two.data(), 2, 0};
    expect(AYNI_INVALID_ARGUMENT);
    fields[0].allowed.unit = text("minute");
    expect(AYNI_INVALID_ARGUMENT);
    calls[0].arguments.fields = nullptr;
    expect(AYNI_INVALID_ARGUMENT);
    const char invalid_utf8[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
    calls[1].text = {invalid_utf8, 2};
    expect(AYNI_INVALID_ARGUMENT);
    calls[1].text = {nullptr, 1};
    expect(AYNI_INVALID_ARGUMENT);
    std::string big(AYNI_CALL_TEXT_BYTES_MAX + 1U, 'a');
    calls[1].text = {big.data(), static_cast<uint32_t>(big.size())};
    expect(AYNI_CAPACITY_EXCEEDED);
}

// Persisted lists are read strictly: a row cannot smuggle a second order or a tail.
TEST_F(ToolCallsTest, decoder_rejects_noncanonical_or_truncated_lists) {
    ASSERT_EQ(encode(1), AYNI_OK);
    const auto valid = body;
    ToolCallView call{};
    EXPECT_EQ(tool_call_count({nullptr, 0}), 0U);
    body.push_back(0);
    EXPECT_EQ(tool_call_count(list()), 0U);
    body = valid;
    body.pop_back();
    EXPECT_EQ(tool_call_count(list()), 0U);
    EXPECT_FALSE(read_tool_call(list(), 0, call));
    body = valid;
    ayni::foundation::store_le32(body.data(), 0);
    EXPECT_EQ(tool_call_count(list()), 0U);
    ayni::foundation::store_le32(body.data(), AYNI_CALL_TOOL_COUNT_MAX + 1U);
    EXPECT_EQ(tool_call_count(list()), 0U);
    body = valid;
    // Swap the two field ids so they descend: offsets follow the format.
    const uint32_t first_field = 4 + 4 + 4 + 9 + 4;
    ASSERT_EQ(ayni::foundation::load_le32(body.data() + first_field), 2U);
    const uint32_t second_field = first_field + 12 + 6 + 8;
    ASSERT_EQ(ayni::foundation::load_le32(body.data() + second_field), 7U);
    ayni::foundation::store_le32(body.data() + second_field, 1U);
    EXPECT_EQ(tool_call_count(list()), 0U);
    body = valid;
    ayni::foundation::store_le32(body.data() + second_field + 4, 9U); // Unknown kind.
    EXPECT_EQ(tool_call_count(list()), 0U);
    body = valid;
    EXPECT_EQ(tool_call_count(list()), 1U);
}
} // namespace
