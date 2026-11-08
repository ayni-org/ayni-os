#pragma once
#include <cstddef>
#include <cstdint>
namespace wifichat::wifi {
// Ephemeral ATVV records. Keeping ADPCM until HTTP upload bounds a 30-second
// recording to 256 KiB instead of retaining a second 960 KiB PCM allocation.
namespace capture {
constexpr size_t capacity = 256 * 1024, header_bytes = 12, record_bytes = 3;
constexpr size_t frame_bytes = 120, frame_samples = frame_bytes * 2;
constexpr size_t sync_bytes = 3, pcm_bytes_per_frame = frame_samples * sizeof(int16_t);
constexpr uint8_t audio = 1, sync = 2;
bool append(uint8_t *out, size_t capacity, size_t &used, uint8_t kind, const uint8_t *data,
            size_t bytes);
void finish(uint8_t *out, size_t bytes, uint32_t pcm_bytes);
} // namespace capture
class CaptureReader {
  public:
    bool initialize(const uint8_t *data, size_t bytes);
    bool next(int16_t *samples, size_t capacity, size_t &count);
    uint32_t pcm_bytes() const { return pcm_bytes_; }
    bool complete() const { return at_ == bytes_; }

  private:
    const uint8_t *data_ = nullptr;
    size_t bytes_ = 0, at_ = 0;
    uint32_t pcm_bytes_ = 0;
    int predictor_ = 0, step_index_ = 0;
};
} // namespace wifichat::wifi
