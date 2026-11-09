#pragma once
#include "../main/speech_profile.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wifichat::wifi {
// AYTS/1 owns framing only. The sink copies audio; HTTP completion is a separate gate.
class SpeechStream {
  public:
    static constexpr uint32_t rate = SpeechProfile::rate,
                              max_pcm_bytes = SpeechProfile::maximum_bytes;
    static constexpr size_t max_frame = 16384, header_bytes = 16, record_bytes = 8;
    using Sink = bool (*)(void *, bool begin, const uint8_t *, size_t);
    SpeechStream(Sink sink, void *context) : sink_(sink), context_(context) {}
    bool feed(const uint8_t *data, size_t bytes) {
        while (bytes && state_ != State::failed) {
            if (state_ == State::ended) {
                return fail();
            }
            const size_t needed = state_ == State::header   ? header_bytes
                                  : state_ == State::record ? record_bytes
                                                            : length_;
            const auto count = bytes < needed - buffered_ ? bytes : needed - buffered_;
            std::memcpy(buffer_.data() + buffered_, data, count);
            data += count;
            bytes -= count;
            buffered_ += count;
            if (buffered_ != needed) {
                continue;
            }
            buffered_ = 0;
            if (state_ == State::header) {
                if (std::memcmp(buffer_.data(), "AYTS", 4) || read32(4) != 1 || read32(8) != rate ||
                    read32(12) != 1 || !sink_(context_, true, nullptr, 0)) {
                    return fail();
                }
                state_ = State::record;
            } else if (state_ == State::record) {
                kind_ = read32(0);
                length_ = read32(4);
                if ((kind_ == 1 && length_ && !(length_ % 2) && length_ <= max_frame &&
                     length_ <= max_pcm_bytes - received_) ||
                    ((kind_ == 2 || kind_ == 3) && length_ == sizeof(uint32_t))) {
                    state_ = State::payload;
                } else {
                    return fail();
                }
            } else {
                if (kind_ == 1) {
                    if (!sink_(context_, false, buffer_.data(), length_)) {
                        return fail();
                    }
                    received_ += length_;
                    state_ = State::record;
                } else if (kind_ == 2 && received_ && read32(0) == received_) {
                    state_ = State::ended;
                } else {
                    if (kind_ == 3) {
                        error_ = read32(0);
                    }
                    return fail();
                }
            }
        }
        return state_ != State::failed;
    }
    bool complete() const { return state_ == State::ended; }
    uint32_t received() const { return received_; }
    uint32_t error() const { return error_; }

  private:
    enum class State { header, record, payload, ended, failed };
    uint32_t read32(size_t at) const {
        return uint32_t(buffer_[at]) | (uint32_t(buffer_[at + 1]) << 8U) |
               (uint32_t(buffer_[at + 2]) << 16U) | (uint32_t(buffer_[at + 3]) << 24U);
    }
    bool fail() {
        state_ = State::failed;
        return false;
    }
    Sink sink_;
    void *context_;
    State state_ = State::header;
    size_t buffered_ = 0;
    uint32_t kind_ = 0, length_ = 0, received_ = 0, error_ = 0;
    std::array<uint8_t, max_frame> buffer_{};
};
} // namespace wifichat::wifi
