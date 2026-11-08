// Experiment-only in-process messages, independent of Ayni ABI/storage versions.
#pragma once
#include <array>
#include <ayni/bodies.h>
#include <cstddef>
#include <cstdint>
namespace wifichat {
constexpr size_t screen_width = 240, screen_height = 320;
constexpr size_t screen_bytes = screen_width * screen_height * sizeof(uint16_t);
constexpr size_t input_bytes = 4096, answer_bytes = 4096;
// A history summary may use all of the output budget; Chinese text can exceed answer_bytes.
constexpr size_t summary_bytes = 8192;
constexpr size_t tx_bytes = 32768;
constexpr size_t packet_slots = 4;
constexpr uint32_t owner_stack_capacity = 96 * 1024;
constexpr size_t body_reference_bytes = 24; // id, digest, size and reserved, little endian.
constexpr size_t model_request_bytes = body_reference_bytes + sizeof(uint32_t);
constexpr size_t tool_outline_bytes = 2048;
enum class Kind : uint8_t {
    hello = 1,
    user = 2,
    model_request = 3,
    model_text = 4,
    model_error = 5,
    answer = 6,
    status = 7,
    display = 8,
    display_ack = 9,
    cancel = 10,
    shutdown = 11,
    configure = 12,
    model_tools = 13,
    render_text = 14,
    render_info = 15,
    speech_request = 16,
    audio_begin = 17,
    audio_chunk = 18,
    audio_end = 19,
    audio_ack = 20,
    audio_error = 21,
    audio_cancel = 22,
    motor_stop = 26,
    motor_settings = 27,
    motor_power_off = 28,
    motor_heartbeat = 30,
    motor_state = 31,
    forget = 32,
    tool_outline = 34
};
// A bounded in-process lane; network protocols are owned by their adapters.
struct Packet {
    Kind kind{};
    uint64_t id = 0;
    uint32_t size = 0;
    std::array<uint8_t, tx_bytes> body{};
};
inline uint32_t get32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8U) | (uint32_t(p[2]) << 16U) |
           (uint32_t(p[3]) << 24U);
}
inline uint64_t get64(const uint8_t *p) {
    return get32(p) | (uint64_t(get32(p + 4)) << 32U);
}
inline void put32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < sizeof(n); ++i) {
        p[i] = uint8_t(n >> (i * 8U));
    }
}
inline void put64(uint8_t *p, uint64_t n) {
    put32(p, uint32_t(n));
    put32(p + sizeof(uint32_t), uint32_t(n >> 32U));
}
inline void put_body_reference(uint8_t *p, const ayni_body_ref_v1 &ref) {
    put64(p, ref.id);
    put64(p + sizeof(uint64_t), ref.digest);
    put32(p + 2 * sizeof(uint64_t), ref.size);
    put32(p + 2 * sizeof(uint64_t) + sizeof(uint32_t), ref.reserved);
}
inline ayni_body_ref_v1 get_body_reference(const uint8_t *p) {
    return {get64(p), get64(p + sizeof(uint64_t)), get32(p + 2 * sizeof(uint64_t)),
            get32(p + 2 * sizeof(uint64_t) + sizeof(uint32_t))};
}
inline uint32_t checksum(const void *data, size_t bytes) {
    auto p = static_cast<const uint8_t *>(data);
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}
} // namespace wifichat
