// A 64-bit digest that tells two byte sequences apart (FNV-1a). It finds a
// command retried with other content (C-WIRE-004); like the log's CRC
// (C-STORE-008) it is no protection against someone choosing a collision,
// which only a caller that already holds the session could exploit, and
// only against its own command.

#ifndef AYNI_CORE_FOUNDATION_DIGEST_H
#define AYNI_CORE_FOUNDATION_DIGEST_H

#include <cstddef>
#include <cstdint>

namespace ayni::foundation {

class Digest {
  public:
    // data may be null only when size is 0.
    void add(const void *data, std::size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (std::size_t index = 0; index < size; ++index) {
            value_ = (value_ ^ bytes[index]) * prime;
        }
    }
    // Fixed width and byte order, so the digest does not depend on the CPU.
    void add_u32(uint32_t value) {
        for (unsigned index = 0; index < 4U; ++index) {
            add_byte(static_cast<uint8_t>(value >> (8U * index)));
        }
    }
    void add_u64(uint64_t value) {
        for (unsigned index = 0; index < 8U; ++index) {
            add_byte(static_cast<uint8_t>(value >> (8U * index)));
        }
    }
    // A length first, so that the bytes of two fields never run together.
    void add_bytes(const void *data, uint32_t size) {
        add_u32(size);
        add(data, size);
    }

    uint64_t value() const { return value_; }

  private:
    static constexpr uint64_t offset_basis = 0xCBF29CE484222325ULL;
    static constexpr uint64_t prime = 0x100000001B3ULL;

    void add_byte(uint8_t byte) { value_ = (value_ ^ byte) * prime; }

    uint64_t value_ = offset_basis;
};

} // namespace ayni::foundation

#endif // AYNI_CORE_FOUNDATION_DIGEST_H
