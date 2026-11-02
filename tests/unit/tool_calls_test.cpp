#include "workspace_fixture.h"
// White-box tool-call list codec, ADR-0054 and tool-call list format 1 in
// contracts/storage/records.md. Shape only; offers and authority are Kernel tests.
#include "catalog/capability/argument_encoding.h"
#include "catalog/capability/value_encoding.h"
#include "foundation/little_endian.h"
#include "work/call/tool_calls.h"
#include <ayni/values.hpp>

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
        nodes = {};
        nodes[0].kind = AYNI_ARGUMENT_OBJECT;
        nodes[0].subtree_end = 4;
        nodes[1].kind = AYNI_ARGUMENT_STRING;
        nodes[1].name = text("mode");
        nodes[1].subtree_end = 2;
        nodes[1].value.text = text("quiet");
        nodes[2].kind = AYNI_ARGUMENT_NUMBER;
        nodes[2].name = text("count");
        nodes[2].subtree_end = 3;
        nodes[2].value.text = text("-5");
        nodes[3].kind = AYNI_ARGUMENT_STRING;
        nodes[3].name = text("body");
        nodes[3].subtree_end = 4;
        nodes[3].value.text = text("早点提醒");
        empty = {};
        empty.kind = AYNI_ARGUMENT_STRING;
        empty.subtree_end = 1;
        calls = {};
        for (auto &call : calls) {
            call.struct_size = sizeof(call);
            call.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
        }
        calls[0].function_name = text("timer.set");
        calls[0].protocol_id = text("t1");
        calls[0].input.arguments = {nodes.data(), static_cast<uint32_t>(nodes.size())};
        calls[1].function_name = text("lamp.off");
        calls[1].protocol_id = text("t2");
        calls[1].input.arguments = {&empty, 1};
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
    std::array<ayni_argument_node_v1, 4> nodes{};
    ayni_argument_node_v1 empty{};
    std::array<ayni_tool_call_v1, 2> calls{};
    std::vector<uint8_t> body;
};

// ADR-0084: producer encoding owns source bytes but does not normalize field
// order. Only the original business Schema owner can fill defaults/canonicalize.
TEST_F(ToolCallsTest, round_trip_keeps_named_arguments_without_retaining_source_views) {
    ASSERT_EQ(encode(), AYNI_OK);
    nodes[3].value.text = text("changed after the post");
    ASSERT_EQ(tool_call_count(::ayni::test::codec_workspace(), list()), 2U);
    ToolCallView call{};
    ASSERT_TRUE(read_tool_call(::ayni::test::codec_workspace(), list(), 0, call));
    EXPECT_EQ(string_of(call.function_name), "timer.set");
    EXPECT_EQ(string_of(call.protocol_id), "t1");
    EXPECT_EQ(call.input_kind, AYNI_TOOL_INPUT_ARGUMENTS);
    ayni::catalog::DecodedArguments value;
    ASSERT_EQ(ayni::catalog::decode_arguments(::ayni::test::codec_workspace(), call.input, value),
              AYNI_OK);
    ASSERT_EQ(value.count, 4U);
    EXPECT_EQ(string_of(value.nodes[1].name), "mode");
    EXPECT_EQ(string_of(value.nodes[1].value.text), "quiet");
    EXPECT_EQ(string_of(value.nodes[2].name), "count");
    EXPECT_EQ(string_of(value.nodes[2].value.text), "-5");
    EXPECT_EQ(string_of(value.nodes[3].value.text), "早点提醒");
    ASSERT_TRUE(read_tool_call(::ayni::test::codec_workspace(), list(), 1, call));
    EXPECT_EQ(string_of(call.function_name), "lamp.off");
    EXPECT_FALSE(read_tool_call(::ayni::test::codec_workspace(), list(), 2, call));
    EXPECT_EQ(string_of(call.function_name), "lamp.off");
}

// C-CALL-014/ADR-0084: invalid tags, identities, native source trees and total
// bytes fail before post reserves a receipt, preserving the measurement output.
TEST_F(ToolCallsTest, provider_shape_errors_are_classified_and_measure_nothing) {
    uint32_t size = 9;
    EXPECT_EQ(measure_tool_calls(calls.data(), 0, size), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(measure_tool_calls(nullptr, 1, size), AYNI_INVALID_ARGUMENT);
    std::array<ayni_tool_call_v1, AYNI_CALL_TOOL_COUNT_MAX + 1U> many{};
    many.fill(calls[1]);
    EXPECT_EQ(measure_tool_calls(many.data(), static_cast<uint32_t>(many.size()), size),
              AYNI_INVALID_ARGUMENT);
    const std::array<ayni_string_v1, AYNI_CALL_TOOL_COUNT_MAX> ids{
        text("a"), text("b"), text("c"), text("d"), text("e"), text("f"), text("g"), text("h")};
    for (uint32_t at = 0; at < ids.size(); ++at) {
        many[at].protocol_id = ids[at];
    }
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
    calls[0].function_name = {};
    expect(AYNI_INVALID_ARGUMENT);
    calls[0].function_name = {nullptr, 1};
    expect(AYNI_INVALID_ARGUMENT);
    nodes[2].subtree_end = 1;
    expect(AYNI_INVALID_ARGUMENT);
    nodes[2].name = {};
    expect(AYNI_INVALID_ARGUMENT);
    nodes[2].kind = AYNI_ARGUMENT_BOOL;
    nodes[2].value.boolean = 2;
    expect(AYNI_INVALID_ARGUMENT);
    nodes[0].reserved = 1;
    expect(AYNI_INVALID_ARGUMENT);
    calls[0].input_kind = 0;
    expect(AYNI_INVALID_ARGUMENT);
    calls[0].input.arguments.nodes = nullptr;
    expect(AYNI_INVALID_ARGUMENT);
    calls[0].protocol_id = {};
    expect(AYNI_INVALID_ARGUMENT);
    calls[1].protocol_id = calls[0].protocol_id;
    expect(AYNI_INVALID_ARGUMENT);
    const char invalid_utf8[] = {static_cast<char>(0xC0), static_cast<char>(0x80)};
    empty.value.text = {invalid_utf8, 2};
    expect(AYNI_INVALID_ARGUMENT);
    empty.value.text = {nullptr, 1};
    expect(AYNI_INVALID_ARGUMENT);
}

// Format 2 requires exact framing and a supported nested body. Old-format
// magic, unknown source kinds, duplicate IDs and trailing bytes are not readers.
TEST_F(ToolCallsTest, persisted_lists_require_exact_framing_and_supported_source_trees) {
    ASSERT_EQ(encode(1), AYNI_OK);
    const auto valid = body;
    body.pop_back();
    EXPECT_EQ(tool_call_count(::ayni::test::codec_workspace(), list()), 0U);
    body = valid;
    body.push_back(0);
    EXPECT_EQ(tool_call_count(::ayni::test::codec_workspace(), list()), 0U);
    body = valid;
    ayni::foundation::store_le32(body.data(), 1);
    EXPECT_EQ(tool_call_count(::ayni::test::codec_workspace(), list()), 0U);
    body = valid;
    constexpr uint32_t list_header_bytes = 16;
    EXPECT_EQ(ayni::foundation::load_le32(body.data() + list_header_bytes), 9U);
    ToolCallView call{};
    ASSERT_TRUE(read_tool_call(::ayni::test::codec_workspace(), list(), 0, call));
    const auto payload_at = static_cast<size_t>(call.input.data - body.data());
    ayni::foundation::store_le32(body.data() + payload_at, 0);
    EXPECT_EQ(tool_call_count(::ayni::test::codec_workspace(), list()), 0U);
    body = valid;
    EXPECT_EQ(tool_call_count(::ayni::test::codec_workspace(), list()), 1U);
}
} // namespace

// ADR-0084/0085: protocol IDs are opaque, retained independently of Kernel IDs;
// payload tags preserve both native values and lossless untyped adapter numbers.
TEST(ToolValueCalls, portable_list_preserves_protocol_identity_raw_errors_and_number_lexemes) {
    // ADR-0085: valid source and malformed JSON are distinct durable proposals.
    std::array<ayni_argument_node_v1, 2> arguments{};
    arguments[0].kind = AYNI_ARGUMENT_OBJECT;
    arguments[0].subtree_end = 2;
    arguments[1].kind = AYNI_ARGUMENT_NUMBER;
    arguments[1].subtree_end = 2;
    arguments[1].name = text("count");
    arguments[1].value.text = text("9007199254740993");
    std::array<ayni_tool_call_v1, 2> calls{};
    for (auto &call : calls) {
        call.struct_size = sizeof(call);
        call.function_name = text("ayni_call_capability");
    }
    calls[0].protocol_id = text("CALL-Original_1");
    calls[0].input_kind = AYNI_TOOL_INPUT_INVALID_ARGUMENTS;
    calls[0].input.invalid_arguments = text("{broken JSON");
    calls[1].protocol_id = text("call_2");
    calls[1].input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
    calls[1].input.arguments = {arguments.data(), static_cast<uint32_t>(arguments.size())};
    uint32_t size = 0;
    ASSERT_EQ(ayni_tool_calls_measure(calls.data(), 2, {}, &size), AYNI_OK);
    std::vector<uint8_t> bytes(size);
    write_tool_calls(calls.data(), 2, bytes.data(), size);
    ToolCallList decoded;
    ASSERT_TRUE(read_tool_calls(::ayni::test::codec_workspace(), {bytes.data(), size}, decoded));
    ASSERT_EQ(decoded.count, 2U);
    EXPECT_EQ(string_of(decoded.calls[0].protocol_id), "CALL-Original_1");
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(decoded.calls[0].input.data),
                          decoded.calls[0].input.size),
              "{broken JSON");
    ayni::catalog::DecodedArguments named;
    ASSERT_EQ(ayni::catalog::decode_arguments(::ayni::test::codec_workspace(),
                                              decoded.calls[1].input, named),
              AYNI_OK);
    EXPECT_EQ(string_of(named.nodes[1].value.text), "9007199254740993");
    calls[1].protocol_id = calls[0].protocol_id;
    size = 99;
    EXPECT_EQ(ayni_tool_calls_measure(calls.data(), 2, {}, &size), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(size, 99U);
    calls[1].protocol_id = {};
    EXPECT_EQ(ayni_tool_calls_measure(calls.data(), 2, {}, &size), AYNI_INVALID_ARGUMENT);
}
