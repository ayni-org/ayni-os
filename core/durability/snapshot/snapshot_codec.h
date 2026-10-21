// Snapshot framing is separate from journal framing. A complete slot is
// usable only when a journal activation names its generation and checksum.

#ifndef AYNI_CORE_DURABILITY_SNAPSHOT_SNAPSHOT_CODEC_H
#define AYNI_CORE_DURABILITY_SNAPSHOT_SNAPSHOT_CODEC_H

#include "durability/batch/batch_codec.h"

#include <cstdint>

namespace ayni::durability {

constexpr uint16_t snapshot_schema = 1;
constexpr uint32_t snapshot_header_size = 72;
constexpr uint32_t snapshot_marker_size = 24;

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

// CRC-32/ISO-HDLC, continued from previous; start with 0.
uint32_t snapshot_crc32(const uint8_t *bytes, uint32_t size, uint32_t previous = 0);

} // namespace ayni::durability

#endif // AYNI_CORE_DURABILITY_SNAPSHOT_SNAPSHOT_CODEC_H
