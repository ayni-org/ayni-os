#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace wifichat::wifi::atvv {
// ATVV v1.0: TX command and CTL notification opcodes are different directions.
inline constexpr uint8_t audio_stop = 0x00, audio_start = 0x04, start_search = 0x08,
                         mic_open = 0x0c, mic_close = 0x0d, htt_reason = 0x03, adpcm_16khz = 0x02;
enum class MicPhase { opening, recording, closing };
enum class StopReason : uint8_t { closed = 0x00, released = 0x02 };
// v1.0 AUDIO_STOP is opcode + reason; it has no stream-ID field.
inline std::optional<StopReason> stop_reason(const uint8_t *data, size_t bytes) {
    if (!data || bytes != 2 || data[0] != audio_stop) {
        return {};
    }
    return static_cast<StopReason>(data[1]);
}
struct VoicePress {
    MicPhase phase;
    uint32_t epoch;   // Bound at MIC_OPEN or the remote's autonomous HTT start.
    int64_t deadline; // Monotonic microseconds; zero while recording.
};
// RC003-MS HTT starts on CTL before HID. A commanded start must instead retain
// the epoch captured at MIC_OPEN; cancellation never re-stamps that old request.
inline std::optional<uint32_t> start_epoch(const std::optional<VoicePress> &pending,
                                           uint32_t current, bool admitted, uint8_t reason) {
    if (!admitted) {
        return {};
    }
    if (pending) {
        return pending->phase == MicPhase::opening && pending->epoch == current
                   ? std::optional<uint32_t>(pending->epoch)
                   : std::nullopt;
    }
    return reason == htt_reason ? std::optional<uint32_t>(current) : std::nullopt;
}
inline std::array<uint8_t, 2> close_command(uint8_t stream) {
    return {mic_close, stream};
}
} // namespace wifichat::wifi::atvv
