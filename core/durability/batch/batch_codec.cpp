#include "durability/batch/batch_codec.h"

#include "foundation/crc32.h"
#include "foundation/little_endian.h"

#include <cstring>

namespace ayni::durability {
namespace {

using foundation::crc32;
using foundation::load_le16;
using foundation::load_le32;
using foundation::load_le64;
using foundation::store_le16;
using foundation::store_le32;
using foundation::store_le64;

constexpr uint8_t batch_magic[4] = {'A', 'Y', 'J', 'B'};
constexpr uint8_t commit_magic[4] = {'A', 'Y', 'J', 'C'};

// Header field offsets.
constexpr std::size_t schema_at = 4;
constexpr std::size_t header_length_at = 6;
constexpr std::size_t writer_epoch_at = 8;
constexpr std::size_t flags_at = 12;
constexpr std::size_t commit_seq_at = 16;
constexpr std::size_t transition_id_at = 24;
constexpr std::size_t record_count_at = 32;
constexpr std::size_t payload_length_at = 36;

uint32_t low_32(uint64_t value) {
    return static_cast<uint32_t>(value & 0xFFFFFFFFU);
}

} // namespace

BatchWriter::BatchWriter(uint8_t *buffer, std::size_t capacity, const BatchIdentity &identity,
                         uint16_t schema)
    : buffer_(buffer), capacity_(capacity), identity_(identity), schema_(schema) {}

bool BatchWriter::add_record(const RecordHeader &header, const uint8_t *body) {
    uint8_t *out = add_record_body(header);
    if (out == nullptr) {
        return false;
    }
    if (header.length > 0U) {
        std::memcpy(out, body, header.length);
    }
    return true;
}

uint8_t *BatchWriter::add_record_body(const RecordHeader &header) {
    // Room stays for the commit marker, so finish() cannot fail afterwards.
    constexpr std::size_t framing = record_header_size + commit_marker_size;
    if (capacity_ < end_ || capacity_ - end_ < framing ||
        capacity_ - end_ - framing < header.length) {
        return nullptr;
    }
    uint8_t *out = buffer_ + end_;
    store_le16(out, header.type);
    store_le16(out + 2, header.version);
    store_le32(out + 4, header.flags);
    store_le32(out + 8, header.length);
    end_ += record_header_size + header.length;
    ++record_count_;
    return out + record_header_size;
}

std::size_t BatchWriter::finish() {
    if (capacity_ < end_ || capacity_ - end_ < commit_marker_size) {
        return 0;
    }
    const std::size_t payload_length = end_ - batch_header_size;
    std::memcpy(buffer_, batch_magic, sizeof(batch_magic));
    store_le16(buffer_ + schema_at, schema_);
    store_le16(buffer_ + header_length_at, static_cast<uint16_t>(batch_header_size));
    store_le32(buffer_ + writer_epoch_at, identity_.writer_epoch);
    store_le32(buffer_ + flags_at, 0);
    store_le64(buffer_ + commit_seq_at, identity_.commit_seq);
    store_le64(buffer_ + transition_id_at, identity_.transition_id);
    store_le32(buffer_ + record_count_at, record_count_);
    store_le32(buffer_ + payload_length_at, static_cast<uint32_t>(payload_length));
    store_le32(buffer_ + batch_header_size - 4U, crc32(buffer_, batch_header_size - 4U));

    uint8_t *marker = buffer_ + end_;
    std::memcpy(marker, commit_magic, sizeof(commit_magic));
    store_le32(marker + 4, low_32(identity_.commit_seq));
    store_le32(marker + 8, crc32(buffer_ + batch_header_size, payload_length));
    return end_ + commit_marker_size;
}

BatchCheck read_batch(const uint8_t *bytes, std::size_t size, BatchView &out) {
    if (size < header_length_at + 2U || std::memcmp(bytes, batch_magic, sizeof(batch_magic)) != 0) {
        return BatchCheck::corrupt;
    }
    const std::size_t header_length = load_le16(bytes + header_length_at);
    if (header_length < batch_header_size || header_length > size) {
        return BatchCheck::corrupt;
    }
    // The CRC comes first, so a damaged schema or flags field reads as
    // corruption rather than as a newer format.
    if (load_le32(bytes + header_length - 4U) != crc32(bytes, header_length - 4U)) {
        return BatchCheck::corrupt;
    }
    if (load_le16(bytes + schema_at) > storage_schema || load_le32(bytes + flags_at) != 0U) {
        return BatchCheck::unsupported;
    }
    const uint32_t payload_length = load_le32(bytes + payload_length_at);
    if (size - header_length < commit_marker_size ||
        size - header_length - commit_marker_size < payload_length) {
        return BatchCheck::corrupt;
    }
    const uint8_t *payload = bytes + header_length;
    const uint8_t *marker = payload + payload_length;
    const uint64_t commit_seq = load_le64(bytes + commit_seq_at);
    if (std::memcmp(marker, commit_magic, sizeof(commit_magic)) != 0 ||
        load_le32(marker + 4) != low_32(commit_seq) ||
        load_le32(marker + 8) != crc32(payload, payload_length)) {
        return BatchCheck::corrupt;
    }
    out.schema = load_le16(bytes + schema_at);
    out.identity.writer_epoch = load_le32(bytes + writer_epoch_at);
    out.identity.commit_seq = commit_seq;
    out.identity.transition_id = load_le64(bytes + transition_id_at);
    out.record_count = load_le32(bytes + record_count_at);
    out.payload = payload;
    out.payload_length = payload_length;
    out.encoded_size = header_length + payload_length + commit_marker_size;
    return BatchCheck::ok;
}

RecordReader::RecordReader(const BatchView &batch)
    : payload_(batch.payload), payload_length_(batch.payload_length),
      remaining_records_(batch.record_count) {}

RecordReader::Next RecordReader::next(RecordHeader &header, const uint8_t *&body) {
    const std::size_t left = payload_length_ - offset_;
    if (remaining_records_ == 0U) {
        return left == 0U ? Next::end : Next::corrupt;
    }
    if (left < record_header_size) {
        return Next::corrupt;
    }
    const uint8_t *in = payload_ + offset_;
    header.type = load_le16(in);
    header.version = load_le16(in + 2);
    header.flags = load_le32(in + 4);
    header.length = load_le32(in + 8);
    if (left - record_header_size < header.length) {
        return Next::corrupt;
    }
    body = in + record_header_size;
    offset_ += record_header_size + header.length;
    --remaining_records_;
    return Next::record;
}

} // namespace ayni::durability
