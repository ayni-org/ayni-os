// CRC-32/ISO-HDLC, the checksum of the storage format
// (contracts/storage/records.md). It detects corruption only and proves
// nothing about who wrote the bytes (C-STORE-008).

#ifndef AYNI_CORE_FOUNDATION_CRC32_H
#define AYNI_CORE_FOUNDATION_CRC32_H

#include <cstddef>
#include <cstdint>

namespace ayni::foundation {

// data may be null only when size is 0.
uint32_t crc32(const uint8_t *data, std::size_t size);

} // namespace ayni::foundation

#endif // AYNI_CORE_FOUNDATION_CRC32_H
