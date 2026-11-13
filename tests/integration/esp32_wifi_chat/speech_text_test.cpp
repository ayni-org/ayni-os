#include "speech_text.h"
#include <ayni/ayni.h>
#include <gtest/gtest.h>
#include <string>
using namespace wifichat;
namespace {
constexpr size_t presentation_bytes = 32768;
struct RenderingCase {
    const char *markdown;
    const char *spoken;
};
} // namespace
// ADR-0101: only automatic presentation is rendered; Markdown structure is not spoken.
TEST(SpeechText, RendersCommonMarkdownStructuresWithoutDeletingLiteralSymbols) {
    const RenderingCase cases[]{
        {"# 标题\n\n**加粗** 和 *斜体*。\n- 第一项\n- 第二项",
         "标题\n加粗 和 斜体。\n第一项\n第二项"},
        {"***嵌套强调***、~~旧内容~~、foo_bar_baz", "嵌套强调、旧内容、foo_bar_baz"},
        {"> 引用\n\n1. 第一条\n2. 第二条\n\n---\n结尾", "引用\n第一条\n第二条\n结尾"},
        {"[文档](https://example.com \"标题\") 和 ![路线图](image.png)", "文档 和 路线图"},
        {"[资料][ref]\n\n[ref]: https://example.com", "资料"},
        {"`a*b`、\\*保留\\*、负数 -20%、2 * 3", "a*b、*保留*、负数 -20%、2 * 3"},
        {"```cpp\na*b;\nx = -20;\n```", "a*b;\nx = -20;"},
        {"| 名称 | 值 |\n| --- | --- |\n| 速度 | -20% |", "名称；值\n速度；-20%"},
        {"- [ ] 待办\n- [x] 完成", "未完成：待办\n已完成：完成"},
        {"甲 &amp; 乙&nbsp;&#x4E2D;&#33; &#x1F30D;", "甲 & 乙 中! 🌍"},
        {"&NotEqualTilde; &unknownEntity; &#0;", "≂̸ &unknownEntity; �"},
        {"**未闭合 和 [链接](未完成", "**未闭合 和 [链接](未完成"},
        {"---\n\n#\n", ""},
        {"", ""},
    };
    for (const auto &item : cases) {
        SCOPED_TRACE(item.markdown);
        SpeechText rendered;
        ASSERT_EQ(rendered.render(item.markdown, presentation_bytes), AYNI_OK);
        EXPECT_EQ(rendered.view(), item.spoken);
        EXPECT_EQ(ayni_validate_utf8({rendered.view().data(), uint32_t(rendered.view().size())}),
                  AYNI_OK);
    }
}
// ADR-0101: bounded output fails explicitly; it never replaces speech with a truncated prefix.
TEST(SpeechText, RejectsInvalidTextAndCapacityWithoutReplacingThePriorResult) {
    SpeechText rendered;
    ASSERT_EQ(rendered.render("原文", presentation_bytes), AYNI_OK);
    EXPECT_EQ(rendered.render("**更长的正文**", 3), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(rendered.view(), "原文");
    EXPECT_EQ(rendered.render(std::string_view("a\0b", 3), presentation_bytes),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(rendered.render(std::string_view("\xff", 1), presentation_bytes),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(rendered.render("text", 0), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(rendered.view(), "原文");
    rendered.clear();
    EXPECT_TRUE(rendered.view().empty());
}
// ADR-0101: prefer natural boundaries without cutting a Unicode scalar or treating decimals as
// sentences.
TEST(SpeechText, PacksSentenceThenWordBoundariesWithinTheSpeakerByteLimit) {
    EXPECT_EQ(speech_chunk_bytes("甲乙。丙丁戊", 15), std::string_view("甲乙。").size());
    EXPECT_EQ(speech_chunk_bytes("甲乙。”丙丁", 12), std::string_view("甲乙。”").size());
    EXPECT_EQ(speech_chunk_bytes("甲乙，丙丁戊", 15), std::string_view("甲乙，").size());
    EXPECT_EQ(speech_chunk_bytes("Hello. Next sentence continues", 16), 7U);
    EXPECT_EQ(speech_chunk_bytes("\"Hi!\" Next sentence", 10), 6U);
    EXPECT_EQ(speech_chunk_bytes("\"Hello.\" Next sentence continues", 16), 9U);
    EXPECT_EQ(speech_chunk_bytes("alpha beta gamma", 12), 11U);
    EXPECT_EQ(speech_chunk_bytes("3.14.2abcdefgh", 6), 6U);
    EXPECT_EQ(speech_chunk_bytes("\"longwordabcdefgh", 5), 5U);
    EXPECT_EQ(speech_chunk_bytes("短答。", 1024), std::string_view("短答。").size());
    EXPECT_EQ(speech_chunk_bytes("甲", 2), 0U);
    EXPECT_EQ(speech_chunk_bytes("", 1024), 0U);
    std::string answer;
    for (size_t at = 0; at < 400; ++at) {
        answer += "🌍界";
    }
    size_t offset = 0;
    while (offset < answer.size()) {
        const auto count = speech_chunk_bytes(std::string_view(answer).substr(offset), 1024);
        ASSERT_GT(count, 0U);
        ASSERT_LE(count, 1024U);
        EXPECT_EQ(ayni_validate_utf8({answer.data() + offset, uint32_t(count)}), AYNI_OK);
        offset += count;
    }
    EXPECT_EQ(offset, answer.size());
}
