#include "foundation/crc32.h"

#include <array>

namespace ayni::foundation {
namespace {

constexpr uint32_t reflected_polynomial = 0xEDB88320U;

// One entry per 4-bit value: 64 bytes of table instead of 1 KiB, at two
// lookups per byte. Batches are small, so size wins over speed here.
constexpr std::array<uint32_t, 16> make_nibble_table() {
    std::array<uint32_t, 16> table{};
    for (uint32_t nibble = 0; nibble < 16U; ++nibble) {
        uint32_t crc = nibble;
        for (int bit = 0; bit < 4; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ reflected_polynomial : crc >> 1U;
        }
        table[nibble] = crc;
    }
    return table;
}

constexpr std::array<uint32_t, 16> nibble_table = make_nibble_table();

} // namespace

uint32_t crc32(const uint8_t *data, std::size_t size, uint32_t previous) {
    uint32_t crc = ~previous;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        crc = (crc >> 4U) ^ nibble_table[crc & 0xFU];
        crc = (crc >> 4U) ^ nibble_table[crc & 0xFU];
    }
    return ~crc;
}

} // namespace ayni::foundation
