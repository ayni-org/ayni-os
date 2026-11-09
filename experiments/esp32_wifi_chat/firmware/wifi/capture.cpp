#include "capture.h"
#include "../main/protocol.h"
#include <algorithm>
#include <cstring>
namespace wifichat::wifi {
namespace {
constexpr uint32_t magic = 0x31565441;
// Standard IMA ADPCM tables; ATVV sends each byte's high nibble first.
constexpr int steps[] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,   21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,   73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,  253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,  876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749, 3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
constexpr int index_delta[] = {-1, -1, -1, -1, 2, 4, 6, 8};
bool valid_record(uint8_t kind, const uint8_t *data, size_t bytes) {
    return (kind == capture::audio && bytes == capture::frame_bytes) ||
           (kind == capture::sync && bytes == capture::sync_bytes && data[2] < std::size(steps));
}
struct Record {
    uint8_t kind = 0;
    const uint8_t *data = nullptr;
    size_t bytes = 0;
};
bool record(const uint8_t *data, size_t bytes, size_t &at, Record &out) {
    if (bytes - at < capture::record_bytes) {
        return false;
    }
    out.kind = data[at];
    out.bytes = size_t(data[at + 1]) | (size_t(data[at + 2]) << 8U);
    at += capture::record_bytes;
    if (out.bytes > bytes - at) {
        return false;
    }
    out.data = data + at;
    at += out.bytes;
    return valid_record(out.kind, out.data, out.bytes);
}
} // namespace
bool capture::append(uint8_t *out, size_t capacity, size_t &used, uint8_t kind, const uint8_t *data,
                     size_t bytes) {
    if (!out || !data || used > capacity || !valid_record(kind, data, bytes) ||
        bytes + record_bytes > capacity - used) {
        return false;
    }
    out[used++] = kind;
    out[used++] = uint8_t(bytes);
    out[used++] = uint8_t(bytes >> 8U);
    std::memcpy(out + used, data, bytes);
    used += bytes;
    return true;
}
void capture::finish(uint8_t *out, size_t bytes, uint32_t pcm_bytes) {
    put32(out, magic);
    put32(out + 4, pcm_bytes);
    put32(out + 8, bytes - header_bytes);
}
bool CaptureReader::initialize(const uint8_t *data, size_t bytes) {
    if (!data || bytes < capture::header_bytes || bytes > capture::capacity ||
        get32(data) != magic || get32(data + 8) != bytes - capture::header_bytes) {
        return false;
    }
    size_t at = capture::header_bytes;
    uint32_t decoded = 0;
    bool synced = false;
    while (at < bytes) {
        Record value{};
        if (!record(data, bytes, at, value)) {
            return false;
        }
        if (value.kind == capture::sync) {
            synced = true;
        } else {
            if (!synced || decoded > capture::maximum_pcm_bytes - capture::pcm_bytes_per_frame) {
                return false;
            }
            decoded += capture::pcm_bytes_per_frame;
        }
    }
    if (!decoded || decoded != get32(data + 4)) {
        return false;
    }
    data_ = data;
    bytes_ = bytes;
    at_ = capture::header_bytes;
    pcm_bytes_ = decoded;
    predictor_ = step_index_ = 0;
    return true;
}
bool CaptureReader::next(int16_t *samples, size_t capacity, size_t &count) {
    if (!data_ || !samples || capacity < capture::frame_samples) {
        return false;
    }
    while (at_ < bytes_) {
        Record value{};
        if (!record(data_, bytes_, at_, value)) {
            return false;
        }
        if (value.kind == capture::sync) {
            predictor_ = int16_t(uint16_t(value.data[0]) | (uint16_t(value.data[1]) << 8U));
            step_index_ = value.data[2];
            continue;
        }
        for (size_t at = 0; at < capture::frame_samples; ++at) {
            const int nibble = (value.data[at / 2] >> (at % 2 ? 0 : 4)) & 15;
            const int step = steps[step_index_];
            int delta = step >> 3;
            if (nibble & 1) {
                delta += step >> 2;
            }
            if (nibble & 2) {
                delta += step >> 1;
            }
            if (nibble & 4) {
                delta += step;
            }
            predictor_ = std::clamp(predictor_ + (nibble & 8 ? -delta : delta), -32768, 32767);
            step_index_ =
                std::clamp(step_index_ + index_delta[nibble & 7], 0, int(std::size(steps)) - 1);
            samples[at] = int16_t(predictor_);
        }
        count = capture::frame_samples;
        return true;
    }
    count = 0;
    return false;
}
} // namespace wifichat::wifi
