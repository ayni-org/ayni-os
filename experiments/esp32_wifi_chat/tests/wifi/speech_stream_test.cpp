#include "speech_stream.h"
#include <gtest/gtest.h>
#include <vector>
namespace {
using wifichat::wifi::SpeechStream;
void word(std::vector<uint8_t> &out, uint32_t n) {
    for (unsigned at = 0; at < sizeof(n); ++at) {
        out.push_back(uint8_t(n >> (at * 8U)));
    }
}
std::vector<uint8_t> message() {
    std::vector<uint8_t> out{'A', 'Y', 'T', 'S'};
    word(out, 1);
    word(out, SpeechStream::rate);
    word(out, 1);
    word(out, 1);
    word(out, 4);
    out.insert(out.end(), {1, 0, 2, 0});
    word(out, 2);
    word(out, 4);
    word(out, 4);
    return out;
}
struct Received {
    bool began = false;
    std::vector<uint8_t> pcm;
    static bool sink(void *context, bool begin, const uint8_t *data, size_t bytes) {
        auto &self = *static_cast<Received *>(context);
        if (begin) {
            self.began = true;
        } else {
            self.pcm.insert(self.pcm.end(), data, data + bytes);
        }
        return true;
    }
};
// AYTS/1: TCP fragmentation never delays a complete audio frame until terminal END.
TEST(SpeechStream, AudioIsDeliveredBeforeTheTerminalAcrossEverySplit) {
    const auto wire = message();
    for (size_t split = 1; split < wire.size(); ++split) {
        Received got;
        SpeechStream parser(Received::sink, &got);
        ASSERT_TRUE(parser.feed(wire.data(), split));
        ASSERT_TRUE(parser.feed(wire.data() + split, wire.size() - split));
        EXPECT_TRUE(got.began);
        EXPECT_EQ(got.pcm, (std::vector<uint8_t>{1, 0, 2, 0}));
        EXPECT_TRUE(parser.complete());
        EXPECT_EQ(parser.received(), 4U);
    }
    Received got;
    SpeechStream parser(Received::sink, &got);
    ASSERT_TRUE(parser.feed(wire.data(), 28));
    EXPECT_EQ(got.pcm.size(), 4U);
    EXPECT_FALSE(parser.complete());
}
// AYTS/1 completion requires an exact final byte count, never a TCP close alone.
TEST(SpeechStream, TerminalAndNoTrailingDataAreRequired) {
    auto wire = message();
    Received got;
    SpeechStream parser(Received::sink, &got);
    ASSERT_TRUE(parser.feed(wire.data(), wire.size() - 1));
    EXPECT_FALSE(parser.complete());
    ASSERT_TRUE(parser.feed(wire.data() + wire.size() - 1, 1));
    const uint8_t junk = 0;
    EXPECT_FALSE(parser.feed(&junk, 1));
    EXPECT_FALSE(parser.complete());
}
// AYTS/1: invalid shape/error terminal is never reported as successful speech.
TEST(SpeechStream, RejectsUnsupportedFormatAndErrorTerminal) {
    for (const size_t at : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(20), size_t(36)}) {
        auto wire = message();
        wire[at] ^= 1;
        Received got;
        SpeechStream parser(Received::sink, &got);
        EXPECT_FALSE(parser.feed(wire.data(), wire.size()));
        EXPECT_FALSE(parser.complete());
    }
    auto wire = message();
    wire[28] = 3;
    Received got;
    SpeechStream parser(Received::sink, &got);
    EXPECT_FALSE(parser.feed(wire.data(), wire.size()));
}
} // namespace
