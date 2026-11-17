// C-SDK-007: fixed, counting and caller-grown output preserve the same JSON and failure rules.
#include <algorithm>
#include <array>
#include <ayni/values_json.hpp>
#include <gtest/gtest.h>
#include <string>
#include <vector>
namespace {
using ayni::json::Writer;
struct Output {
    std::vector<char> bytes;
    size_t limit = 256;
    uint32_t calls = 0;
    static Writer::Buffer grow(void *context, size_t minimum) {
        auto &self = *static_cast<Output *>(context);
        ++self.calls;
        if (minimum > self.limit) {
            return {};
        }
        constexpr size_t initial_bytes = 8, factor = 2;
        const auto capacity = std::min(
            self.limit, std::max(minimum, std::max(initial_bytes, self.bytes.size() * factor)));
        std::vector<char> next(capacity);
        std::copy(self.bytes.begin(), self.bytes.end(), next.begin());
        self.bytes.swap(next); // Exercise relocation, not just a permanently fixed array.
        return {self.bytes.data(), self.bytes.size()};
    }
};
void document(Writer &out) {
    constexpr char text[] = u8"中\n\"\\\0x";
    out.raw("{\"text\":");
    out.string(ayni_string_v1{text, sizeof(text) - 1U});
    out.raw(",\"number\":");
    out.unsigned_number(UINT64_MAX);
    out.raw("}");
}
TEST(JsonWriter, growing_output_matches_fixed_and_measured_bytes_and_characters) {
    auto measured = Writer::measuring();
    document(measured);
    size_t expected = 0;
    ASSERT_TRUE(measured.finish(expected));
    std::vector<char> fixed(expected);
    Writer bounded(fixed.data(), fixed.size());
    document(bounded);
    size_t bounded_size = 0;
    ASSERT_TRUE(bounded.finish(bounded_size));
    Output output;
    auto grown = Writer::growing(&output, Output::grow);
    document(grown);
    size_t grown_size = 0;
    ASSERT_TRUE(grown.finish(grown_size));
    EXPECT_EQ(grown_size, expected);
    EXPECT_EQ(bounded_size, expected);
    EXPECT_TRUE(std::equal(fixed.begin(), fixed.end(), output.bytes.begin()));
    EXPECT_EQ(grown.characters(), measured.characters());
    EXPECT_EQ(grown.characters(), bounded.characters());
    EXPECT_GT(output.calls, 1U);
}
TEST(JsonWriter, refused_growth_keeps_failure_sticky_and_never_appends_afterwards) {
    Output output;
    output.limit = 4;
    auto writer = Writer::growing(&output, Output::grow);
    writer.raw("abcd");
    writer.raw("e");
    const auto calls = output.calls;
    writer.raw("ignored");
    size_t bytes = 0;
    EXPECT_FALSE(writer.finish(bytes));
    EXPECT_EQ(bytes, output.limit);
    EXPECT_EQ(output.calls, calls);
    EXPECT_EQ(std::string(output.bytes.begin(), output.bytes.end()), "abcd");
}
TEST(JsonWriter, invalid_growth_descriptor_is_refused_before_writing) {
    std::array<char, 4> buffer{};
    auto writer = Writer::growing(&buffer, [](void *context, size_t minimum) {
        return Writer::Buffer{static_cast<std::array<char, 4> *>(context)->data(), minimum - 1U};
    });
    writer.raw("x");
    size_t bytes = 0;
    EXPECT_FALSE(writer.finish(bytes));
    EXPECT_EQ(bytes, 0U);
    EXPECT_EQ(buffer.front(), '\0');
    auto missing = Writer::growing(nullptr, nullptr);
    EXPECT_FALSE(missing.finish(bytes));
}
} // namespace
