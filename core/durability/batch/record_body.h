// Fields of a record body (contracts/storage/records.md): fixed-width
// little-endian integers and byte strings with a uint32 length prefix
// (C-VER-023). The owner of a record type decides its fields and their
// order; these only keep every read and write inside the body.

#ifndef AYNI_CORE_DURABILITY_BATCH_RECORD_BODY_H
#define AYNI_CORE_DURABILITY_BATCH_RECORD_BODY_H

#include "foundation/little_endian.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ayni::durability {

// The bytes a length-prefixed string of size bytes takes.
constexpr uint32_t string_field_size(uint32_t size) {
    return 4U + size;
}

// Writes a body of a size its owner computed beforehand; the owner's size
// function and its writes must agree, so a write past the end is a defect
// and is dropped rather than overrunning the batch.
class BodyWriter {
  public:
    BodyWriter(uint8_t *out, uint32_t size) : out_(out), size_(size) {}

    void u32(uint32_t value) {
        if (fits(4U)) {
            foundation::store_le32(out_ + at_, value);
        }
        at_ += 4U;
    }
    void u64(uint64_t value) {
        if (fits(8U)) {
            foundation::store_le64(out_ + at_, value);
        }
        at_ += 8U;
    }
    // data may be null only when size is 0.
    void bytes(const void *data, uint32_t size) {
        u32(size);
        if (size > 0U && fits(size)) {
            std::memcpy(out_ + at_, data, size);
        }
        at_ += size;
    }

    // Whether exactly the whole body was written.
    bool complete() const { return at_ == size_; }

  private:
    bool fits(uint32_t size) const { return at_ <= size_ && size_ - at_ >= size; }

    uint8_t *out_;
    uint32_t size_;
    uint32_t at_ = 0;
};

// Reads a body field by field. A read past the end fails the reader and
// yields zeros, so a decoder checks ok() once, at the end.
class BodyReader {
  public:
    BodyReader(const uint8_t *in, uint32_t size) : in_(in), size_(size) {}

    uint32_t u32() {
        if (!take(4U)) {
            return 0;
        }
        return foundation::load_le32(in_ + at_ - 4U);
    }
    uint64_t u64() {
        if (!take(8U)) {
            return 0;
        }
        return foundation::load_le64(in_ + at_ - 8U);
    }
    // A string of at most max_size bytes; its bytes stay in the body.
    // Fails the reader for a longer one.
    const uint8_t *bytes(uint32_t max_size, uint32_t &out_size) {
        out_size = u32();
        if (out_size > max_size || !take(out_size)) {
            ok_ = false;
            out_size = 0;
            return nullptr;
        }
        return in_ + at_ - out_size;
    }

    // Remaining framed bytes; malformed reads expose no optional suffix.
    uint32_t remaining() const { return ok_ ? size_ - at_ : 0; }
    // Every read stayed inside the body and the body has nothing left.
    bool ok() const { return ok_ && at_ == size_; }

  private:
    bool take(uint32_t size) {
        if (!ok_ || size_ - at_ < size) {
            ok_ = false;
            return false;
        }
        at_ += size;
        return true;
    }

    const uint8_t *in_;
    uint32_t size_;
    uint32_t at_ = 0;
    bool ok_ = true;
};

} // namespace ayni::durability

#endif // AYNI_CORE_DURABILITY_BATCH_RECORD_BODY_H
