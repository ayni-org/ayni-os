#include "pcm_buffer.h"
#include <array>
#include <gtest/gtest.h>
namespace {
using namespace wifichat;
// ADR-0097: five-minute speech wraps a fixed ring with zero-acceptance backpressure.
TEST(PcmBuffer, FiveMinuteBudgetPreservesOrderWithBoundedStorage) {
    std::array<uint8_t, SpeechProfile::buffer_bytes> storage{};
    std::array<int16_t, 1024> incoming{};
    std::array<int16_t, 256> outgoing{};
    PcmBuffer ring;
    ring.attach(storage.data());
    uint32_t written = 0, read = 0;
    const auto total = SpeechProfile::maximum_bytes / sizeof(int16_t);
    bool full = false;
    while (read < total) {
        if (written < total) {
            const auto count = std::min(incoming.size(), size_t(total - written));
            for (size_t at = 0; at < count; ++at) {
                incoming[at] = int16_t((written + at) % 30000);
            }
            const auto before = ring.received();
            const auto status = ring.append(written * sizeof(int16_t),
                                            reinterpret_cast<const uint8_t *>(incoming.data()),
                                            count * sizeof(int16_t));
            ASSERT_NE(status, AudioWrite::rejected);
            if (status == AudioWrite::accepted) {
                written += count;
            } else {
                full = true;
                ASSERT_EQ(ring.received(), before);
            }
        }
        const auto count = ring.read(outgoing.data(), outgoing.size());
        ASSERT_GT(count, 0U);
        for (size_t at = 0; at < count; ++at) {
            ASSERT_EQ(outgoing[at], int16_t((read + at) % 30000));
        }
        read += count;
    }
    EXPECT_TRUE(full);
    EXPECT_EQ(ring.received(), SpeechProfile::maximum_bytes);
    EXPECT_EQ(ring.available(), 0U);
    EXPECT_EQ(ring.append(SpeechProfile::maximum_bytes, storage.data(), sizeof(int16_t)),
              AudioWrite::rejected);
    EXPECT_EQ(ring.received(), SpeechProfile::maximum_bytes);
    EXPECT_EQ(ring.detach(), storage.data());
    EXPECT_EQ(ring.append(0, storage.data(), 2), AudioWrite::rejected);
}
// ADR-0097: stale offsets are rejected, never retried as accepted audio.
TEST(PcmBuffer, RejectsStaleOffsetsWithoutChangingTheCommittedPrefix) {
    std::array<uint8_t, SpeechProfile::buffer_bytes> storage{};
    PcmBuffer ring;
    ring.attach(storage.data());
    const uint8_t pcm[] = {1, 0};
    ASSERT_EQ(ring.append(0, pcm, sizeof(pcm)), AudioWrite::accepted);
    EXPECT_EQ(ring.append(0, pcm, sizeof(pcm)), AudioWrite::rejected);
    EXPECT_EQ(ring.received(), sizeof(pcm));
}
} // namespace
