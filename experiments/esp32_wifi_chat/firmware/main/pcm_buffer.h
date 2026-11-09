#pragma once
#include "speech_profile.h"
#include <algorithm>
#include <cstring>
namespace wifichat {
// Borrowed fixed PSRAM ring. Speaker owns allocation and locks every method call.
// read copies before consuming, so the producer cannot overwrite a worker's input.
class PcmBuffer {
  public:
    void attach(uint8_t *storage) {
        data_ = storage;
        received_ = consumed_ = 0;
    }
    uint8_t *detach() {
        auto *data = data_;
        data_ = nullptr;
        received_ = consumed_ = 0;
        return data;
    }
    uint32_t received() const { return received_; }
    uint32_t available() const { return received_ - consumed_; }
    AudioWrite append(uint32_t offset, const uint8_t *data, size_t bytes) {
        if (!data_ || !data || !bytes || bytes % sizeof(int16_t) || offset != received_ ||
            bytes > SpeechProfile::buffer_bytes ||
            bytes > SpeechProfile::maximum_bytes - received_) {
            return AudioWrite::rejected;
        }
        if (bytes > SpeechProfile::buffer_bytes - available()) {
            return AudioWrite::full;
        }
        const size_t at = received_ % SpeechProfile::buffer_bytes;
        const auto first = std::min(bytes, SpeechProfile::buffer_bytes - at);
        std::memcpy(data_ + at, data, first);
        std::memcpy(data_, data + first, bytes - first);
        received_ += uint32_t(bytes);
        return AudioWrite::accepted;
    }
    size_t read(int16_t *out, size_t samples) {
        const auto bytes = std::min(samples * sizeof(int16_t), size_t(available()));
        if (!data_ || !bytes) {
            return 0;
        }
        const size_t at = consumed_ % SpeechProfile::buffer_bytes;
        const auto first = std::min(bytes, SpeechProfile::buffer_bytes - at);
        std::memcpy(out, data_ + at, first);
        std::memcpy(reinterpret_cast<uint8_t *>(out) + first, data_, bytes - first);
        consumed_ += uint32_t(bytes);
        return bytes / sizeof(int16_t);
    }

  private:
    uint8_t *data_ = nullptr;              // None until attach; detach ends this borrow.
    uint32_t received_ = 0, consumed_ = 0; // Bytes, monotonic, consumed <= received <= maximum.
};
} // namespace wifichat
