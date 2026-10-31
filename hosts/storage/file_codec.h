// Shared Host file codecs. CRC-32/ISO-HDLC and little-endian encoding; no domain records.
#ifndef AYNI_HOST_FILE_CODEC_H
#define AYNI_HOST_FILE_CODEC_H
#include <array>
#include <cstddef>
#include <cstdint>
namespace ayni::hosts::storage::codec {
constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t index = 0; index < table.size(); ++index) {
        uint32_t crc = index;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? 0xEDB88320U ^ (crc >> 1U) : crc >> 1U;
        }
        table[index] = crc;
    }
    return table;
}

inline constexpr std::array<uint32_t, 256> crc_table = make_crc_table();

// CRC-32/ISO-HDLC; passing a previous result continues it over more bytes.
inline uint32_t crc32(const uint8_t *data, std::size_t size, uint32_t previous = 0) {
    uint32_t crc = ~previous;
    for (std::size_t index = 0; index < size; ++index) {
        crc = crc_table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

inline void store_le32(uint8_t *out, uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        out[index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

inline void store_le64(uint8_t *out, uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        out[index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

inline uint32_t load_le32(const uint8_t *in) {
    uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<uint32_t>(in[index]) << (8U * index);
    }
    return value;
}

inline uint64_t load_le64(const uint8_t *in) {
    uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<uint64_t>(in[index]) << (8U * index);
    }
    return value;
}

} // namespace ayni::hosts::storage::codec
#endif
