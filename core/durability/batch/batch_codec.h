// Commit batch encoding (contracts/storage/records.md, C-STORE-001,
// C-STORE-004). The codec checks framing, versions and checksums; what a
// record body means is up to the module that owns the record type.
//
// Layout: a 44-byte header (header_crc in its last 4 bytes), the records,
// then a 12-byte commit marker binding the payload CRC to commit_seq.

#ifndef AYNI_CORE_DURABILITY_BATCH_BATCH_CODEC_H
#define AYNI_CORE_DURABILITY_BATCH_BATCH_CODEC_H

#include "durability/batch/record_types.h"

#include <ayni/storage_format.h>

#include <cstddef>
#include <cstdint>

namespace ayni::durability {

// Schema 3 is the development boundary (ADR-0082). Other schemas
// are rejected before recovery can write; no old data is implicitly adopted.
constexpr uint16_t storage_schema = 3;
// WRITER_FENCE: alone in its batch, the body is the new writer_epoch as a
// little-endian uint32 equal to the batch header's (C-STORE-005).
constexpr uint16_t writer_fence_record_type = record_type::writer_fence;
constexpr uint16_t writer_fence_record_version = 1;
constexpr uint32_t writer_fence_record_length = 4;
// SNAPSHOT_COMMIT: alone in its batch; durability/snapshot has its body.
constexpr uint16_t snapshot_commit_record_type = record_type::snapshot_commit;
constexpr std::size_t batch_header_size = 44;
constexpr std::size_t record_header_size = 12;
constexpr std::size_t commit_marker_size = 12;
// record_flags bit 0: a reader that does not know the record must not go on.
constexpr uint32_t record_required = 0x1U;

// Every transition takes the next id, so no writer reaches 2^63: at a
// million transitions a second that takes 290,000 years. A larger id in a
// log or a snapshot means it is damaged or foreign, and the bound leaves a
// boot more ids than it can use, so the transition clock never wraps
// (ADR-0017).
constexpr uint64_t transition_id_max = (uint64_t{1} << 63U) - 1U;

constexpr std::size_t batch_size(std::size_t record_body_bytes, std::size_t record_count) {
    return batch_header_size + record_count * record_header_size + record_body_bytes +
           commit_marker_size;
}

// The public value type shares the format owner with Host prefix inspection.
using BatchIdentity = ayni_storage_batch_identity_v1;

struct RecordHeader {
    uint16_t type;
    uint16_t version;
    uint32_t flags;
    uint32_t length;
};

// The record envelope, record_header_size bytes, shared by batches and
// snapshot payloads (contracts/storage/records.md). Neither checks length.
void write_record_header(const RecordHeader &header, uint8_t *out);
RecordHeader read_record_header(const uint8_t *bytes);

// Encodes one batch into caller memory: add the records, then finish().
class BatchWriter {
  public:
    BatchWriter(uint8_t *buffer, std::size_t capacity, const BatchIdentity &identity);

    // false, with nothing written, when the record and the commit marker
    // after it do not fit. body may be null only when header.length is 0.
    bool add_record(const RecordHeader &header, const uint8_t *body);
    // As add_record, but returns where the caller writes the header.length
    // body bytes; null, with nothing written, when they do not fit.
    uint8_t *add_record_body(const RecordHeader &header);

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
    no_room,     // a record's owner has no room for it (prefix_scan.h)
};

// A checked batch; payload points into the bytes given to read_batch.
struct BatchView {
    BatchIdentity identity;
    uint16_t schema;
    uint32_t record_count;
    const uint8_t *payload;
    uint32_t payload_length;
    std::size_t encoded_size;
};

// Checks the batch at the start of bytes. Only the current fixed header is
// accepted; a valid CRC cannot make an unknown extension readable (ADR-0082).
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
