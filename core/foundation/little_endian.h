// Fixed-width little-endian fields of the storage format
// (contracts/storage/records.md). Persistent bytes never depend on the
// compiler's memory layout or the CPU's byte order (C-VER-023).

#ifndef AYNI_CORE_FOUNDATION_LITTLE_ENDIAN_H
#define AYNI_CORE_FOUNDATION_LITTLE_ENDIAN_H

#include <cstdint>

namespace ayni::foundation {

inline void store_le16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8U);
}

inline void store_le32(uint8_t *out, uint32_t value) {
    for (unsigned index = 0; index < 4U; ++index) {
        out[index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

inline void store_le64(uint8_t *out, uint64_t value) {
    for (unsigned index = 0; index < 8U; ++index) {
        out[index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

inline uint16_t load_le16(const uint8_t *in) {
    return static_cast<uint16_t>(in[0] | (in[1] << 8U));
}

inline uint32_t load_le32(const uint8_t *in) {
    uint32_t value = 0;
    for (unsigned index = 0; index < 4U; ++index) {
        value |= static_cast<uint32_t>(in[index]) << (8U * index);
    }
    return value;
}

inline uint64_t load_le64(const uint8_t *in) {
    uint64_t value = 0;
    for (unsigned index = 0; index < 8U; ++index) {
        value |= static_cast<uint64_t>(in[index]) << (8U * index);
    }
    return value;
}

} // namespace ayni::foundation

#endif // AYNI_CORE_FOUNDATION_LITTLE_ENDIAN_H
