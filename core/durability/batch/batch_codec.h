// Commit batch encoding (contracts/storage/records.md, C-STORE-001,
// C-STORE-004). The codec checks framing, versions and checksums; what a
// record body means is up to the module that owns the record type.
//
// Layout: a 44-byte header (header_crc in its last 4 bytes), the records,
// then a 12-byte commit marker binding the payload CRC to commit_seq.

#ifndef AYNI_CORE_DURABILITY_BATCH_BATCH_CODEC_H
#define AYNI_CORE_DURABILITY_BATCH_BATCH_CODEC_H

#include <cstddef>
#include <cstdint>

namespace ayni::durability {

// Experimental until fault injection settles the layout (records.md).
constexpr uint16_t storage_schema = 0;
constexpr std::size_t batch_header_size = 44;
constexpr std::size_t record_header_size = 12;
constexpr std::size_t commit_marker_size = 12;
// record_flags bit 0: a reader that does not know the record must not go on.
constexpr uint32_t record_required = 0x1U;

constexpr std::size_t batch_size(std::size_t record_body_bytes, std::size_t record_count) {
    return batch_header_size + record_count * record_header_size + record_body_bytes +
           commit_marker_size;
}

struct BatchIdentity {
    uint32_t writer_epoch;
    uint64_t commit_seq;
    uint64_t transition_id;
};

struct RecordHeader {
    uint16_t type;
    uint16_t version;
    uint32_t flags;
    uint32_t length;
};

// Encodes one batch into caller memory: add the records, then finish().
class BatchWriter {
  public:
    BatchWriter(uint8_t *buffer, std::size_t capacity, const BatchIdentity &identity);

    // false, with nothing written, when the record and the commit marker
    // after it do not fit. body may be null only when header.length is 0.
    bool add_record(const RecordHeader &header, const uint8_t *body);

    // Writes the header and commit marker and returns the encoded size; 0
    // only when capacity cannot hold even an empty batch.
    std::size_t finish();

  private:
    uint8_t *buffer_;
    std::size_t capacity_;
    BatchIdentity identity_;
    std::size_t end_ = batch_header_size;
    uint32_t record_count_ = 0;
};

enum class BatchCheck {
    ok,
    corrupt,     // cut short, or a magic, CRC or commit marker does not match
    unsupported, // another storage_schema or unknown flag bits (C-STORE-004)
};

// A checked batch; payload points into the bytes given to read_batch.
struct BatchView {
    BatchIdentity identity;
    uint32_t record_count;
    const uint8_t *payload;
    uint32_t payload_length;
    std::size_t encoded_size;
};

// Checks the batch at the start of bytes. A header longer than 44 bytes is
// accepted: fields appended by a later writer sit before header_crc.
BatchCheck read_batch(const uint8_t *bytes, std::size_t size, BatchView &out);

// Walks the records of a checked batch.
class RecordReader {
  public:
    enum class Next { record, end, corrupt };

    explicit RecordReader(const BatchView &batch);

    // corrupt when a record overruns the payload, or the records do not fill
    // it exactly as record_count says.
    Next next(RecordHeader &header, const uint8_t *&body);

  private:
    const uint8_t *payload_;
    uint32_t payload_length_;
    uint32_t remaining_records_;
    std::size_t offset_ = 0;
};

} // namespace ayni::durability

#endif // AYNI_CORE_DURABILITY_BATCH_BATCH_CODEC_H
