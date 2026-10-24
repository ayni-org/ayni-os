// Snapshot slot framing and the SNAPSHOT_COMMIT record that activates a slot
// (contracts/storage/records.md, C-STORE-006, ADR-0027 decisions 7 and 8).
//
// A slot holds a 72-byte header, the payload records and a 24-byte marker.
// The marker only proves the slot was written whole; a slot is in use only
// once a SNAPSHOT_COMMIT in the log names its generation and payload CRC.
// What the payload records mean is up to the kernel (work_snapshot.h).

#ifndef AYNI_CORE_DURABILITY_SNAPSHOT_SNAPSHOT_CODEC_H
#define AYNI_CORE_DURABILITY_SNAPSHOT_SNAPSHOT_CODEC_H

#include "durability/batch/batch_codec.h"

#include <cstdint>

namespace ayni::durability {

constexpr uint16_t snapshot_schema = 2;
constexpr uint32_t snapshot_header_size = 72;
constexpr uint32_t snapshot_marker_size = 24;

// What a slot header carries. covers_commit_seq is the last batch the
// snapshot holds; the log resumes after it.
struct SnapshotMetadata {
    uint16_t schema = snapshot_schema;
    uint64_t generation = 0;
    uint64_t covers_commit_seq = 0;
    uint64_t max_transition_id = 0;
    uint64_t boot_epoch = 0;
    uint32_t writer_epoch = 0;
    uint64_t payload_length = 0;
    uint32_t record_count = 0;
    uint32_t payload_crc = 0;
};

// Buffers hold the corresponding fixed size. Header integrity is checked
// before version support; a damaged schema is corruption, not an upgrade.
void write_snapshot_header(const SnapshotMetadata &metadata, uint8_t *out);
BatchCheck read_snapshot_header(const uint8_t *bytes, uint32_t size, SnapshotMetadata &out);
void write_snapshot_marker(const SnapshotMetadata &metadata, uint8_t *out);
BatchCheck read_snapshot_marker(const uint8_t *bytes, uint32_t size,
                                const SnapshotMetadata &metadata);

// CRC-32/ISO-HDLC, continued from previous; start with 0. The payload is
// written and read a page at a time, so its CRC is carried across pages.
uint32_t snapshot_crc32(const uint8_t *bytes, uint32_t size, uint32_t previous = 0);

// SNAPSHOT_COMMIT body. Its batch is the one after the snapshot: commit_seq
// covers_commit_seq + 1 and transition_id max_transition_id + 1.
constexpr uint16_t snapshot_commit_record_version = 1;
constexpr uint32_t snapshot_commit_record_length = 32;
// The batch that holds it alone: all a trimmed log keeps before new batches.
constexpr uint32_t snapshot_commit_batch_size =
    static_cast<uint32_t>(batch_size(snapshot_commit_record_length, 1));

struct SnapshotActivation {
    uint32_t slot = 0;
    uint32_t payload_crc = 0;
    uint64_t generation = 0;
    uint64_t covers_commit_seq = 0;
    uint64_t max_transition_id = 0;
};

// body has snapshot_commit_record_length bytes.
void write_snapshot_activation(const SnapshotActivation &activation, uint8_t *body);
// false for another length or a body this library cannot have written.
bool read_snapshot_activation(const uint8_t *body, uint32_t size, SnapshotActivation &out);
// Whether a slot header is the one the activation names.
bool activation_matches(const SnapshotActivation &activation, const SnapshotMetadata &metadata);

} // namespace ayni::durability

#endif // AYNI_CORE_DURABILITY_SNAPSHOT_SNAPSHOT_CODEC_H
