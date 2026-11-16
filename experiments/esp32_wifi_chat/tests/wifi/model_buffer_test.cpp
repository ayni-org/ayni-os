#include "model_buffer.h"
#include <ayni/values_json.hpp>
#include <gtest/gtest.h>
#include <string>
namespace {
// ADR-0107: request encoding grows only bounded PSRAM, preserves bytes on relocation,
// and erases replaced/final lanes. This desktop spy does not measure ESP heap peaks.
TEST(ModelBuffer, growing_json_preserves_utf8_and_erases_every_released_lane) {
    heap_spy::erased = heap_spy::psram_only = true;
    constexpr size_t maximum = 8192;
    const std::string text(3000, 'x');
    {
        wifichat::wifi::ModelBuffer memory;
        auto out = ayni::json::Writer::growing(&memory, [](void *context, size_t minimum) {
            auto &buffer = *static_cast<wifichat::wifi::ModelBuffer *>(context);
            return buffer.grow(minimum, maximum)
                       ? ayni::json::Writer::Buffer{reinterpret_cast<char *>(buffer.data),
                                                    buffer.size}
                       : ayni::json::Writer::Buffer{};
        });
        out.raw("[\"你好\",");
        out.string(text);
        out.raw("]");
        size_t bytes = 0;
        ASSERT_TRUE(out.finish(bytes));
        EXPECT_EQ(std::string(reinterpret_cast<char *>(memory.data), bytes),
                  "[\"你好\",\"" + text + "\"]");
        EXPECT_LE(memory.size, maximum);
        const auto *before = memory.data;
        EXPECT_FALSE(memory.grow(maximum + 1, maximum));
        EXPECT_EQ(memory.data, before);
    }
    EXPECT_TRUE(heap_spy::erased);
    EXPECT_TRUE(heap_spy::psram_only);
    EXPECT_TRUE(heap_spy::allocations.empty());
}
// ADR-0107: an output beyond the configured bound fails without publishing truncated JSON.
TEST(ModelBuffer, writer_refuses_output_over_the_capacity_bound) {
    wifichat::wifi::ModelBuffer memory;
    auto out = ayni::json::Writer::growing(&memory, [](void *context, size_t minimum) {
        auto &buffer = *static_cast<wifichat::wifi::ModelBuffer *>(context);
        constexpr size_t maximum = 64;
        return buffer.grow(minimum, maximum)
                   ? ayni::json::Writer::Buffer{reinterpret_cast<char *>(buffer.data), buffer.size}
                   : ayni::json::Writer::Buffer{};
    });
    out.string(std::string(65, 'x'));
    size_t bytes = 0;
    EXPECT_FALSE(out.finish(bytes));
    EXPECT_LE(memory.size, 64U);
}
} // namespace
