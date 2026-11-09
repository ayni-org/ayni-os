#pragma once
#include <cstddef>
#include <cstdint>
namespace wifichat {
// Experiment wire/playback budget. ASR recording has its own independent limit.
struct SpeechProfile {
    static constexpr uint32_t rate = 24000, maximum_seconds = 300;
    static constexpr uint32_t maximum_bytes = rate * maximum_seconds * sizeof(int16_t);
    static constexpr uint32_t timeout_ms = (maximum_seconds + 30) * 1000;
    static constexpr size_t buffer_bytes = 64 * 1024;
    static constexpr uint32_t capability_version = 2;
};
// full means zero bytes accepted, allowing an I/O worker to wait without replaying audio.
enum class AudioWrite { accepted, full, rejected };
} // namespace wifichat
