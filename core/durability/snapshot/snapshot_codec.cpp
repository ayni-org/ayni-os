#include "durability/snapshot/snapshot_codec.h"

#include "foundation/crc32.h"
#include "foundation/little_endian.h"

#include <cstring>

namespace ayni::durability {
namespace {

constexpr uint8_t magic[4] = {'A', 'Y', 'S', 'N'};
constexpr uint8_t marker_magic[4] = {'A', 'Y', 'S', 'C'};

} // namespace

uint32_t snapshot_crc32(const uint8_t *bytes, uint32_t size, uint32_t previous) {
    return foundation::crc32(bytes, size, previous);
}

void write_snapshot_header(const SnapshotMetadata &metadata, uint8_t *out) {
    std::memset(out, 0, snapshot_header_size);
    std::memcpy(out, magic, sizeof(magic));
    foundation::store_le16(out + 4, metadata.schema);
    foundation::store_le16(out + 6, snapshot_header_size);
    foundation::store_le64(out + 8, metadata.generation);
    foundation::store_le64(out + 16, metadata.covers_commit_seq);
    foundation::store_le64(out + 24, metadata.max_transition_id);
    foundation::store_le64(out + 32, metadata.boot_epoch);
    foundation::store_le32(out + 40, metadata.writer_epoch);
    foundation::store_le64(out + 48, metadata.payload_length);
    foundation::store_le32(out + 56, metadata.record_count);
    foundation::store_le32(out + 60, metadata.payload_crc);
    foundation::store_le32(out + 68, foundation::crc32(out, 68));
}

BatchCheck read_snapshot_header(const uint8_t *bytes, uint32_t size, SnapshotMetadata &out) {
    if (size != snapshot_header_size || std::memcmp(bytes, magic, sizeof(magic)) != 0 ||
        foundation::load_le16(bytes + 6) != snapshot_header_size ||
        foundation::load_le32(bytes + 68) != foundation::crc32(bytes, 68)) {
        return BatchCheck::corrupt;
    }
    if (foundation::load_le16(bytes + 4) != snapshot_schema ||
        foundation::load_le32(bytes + 44) != 0U || foundation::load_le32(bytes + 64) != 0U) {
        return BatchCheck::unsupported;
    }
    SnapshotMetadata metadata{};
    metadata.schema = foundation::load_le16(bytes + 4);
    metadata.generation = foundation::load_le64(bytes + 8);
    metadata.covers_commit_seq = foundation::load_le64(bytes + 16);
    metadata.max_transition_id = foundation::load_le64(bytes + 24);
    metadata.boot_epoch = foundation::load_le64(bytes + 32);
    metadata.writer_epoch = foundation::load_le32(bytes + 40);
    metadata.payload_length = foundation::load_le64(bytes + 48);
    metadata.record_count = foundation::load_le32(bytes + 56);
    metadata.payload_crc = foundation::load_le32(bytes + 60);
    if (metadata.generation == 0U || metadata.writer_epoch == 0U ||
        metadata.boot_epoch > UINT32_MAX || metadata.max_transition_id > (UINT64_MAX >> 1U) ||
        metadata.covers_commit_seq > metadata.max_transition_id ||
        (metadata.record_count == 0U) != (metadata.payload_length == 0U) ||
        metadata.payload_length < uint64_t{metadata.record_count} * record_header_size ||
        (metadata.covers_commit_seq == 0U &&
         (metadata.max_transition_id != 0U || metadata.boot_epoch != 0U ||
          metadata.record_count != 0U))) {
        return BatchCheck::corrupt;
    }
    out = metadata;
    return BatchCheck::ok;
}

void write_snapshot_marker(const SnapshotMetadata &metadata, uint8_t *out) {
    uint8_t header[snapshot_header_size]{};
    write_snapshot_header(metadata, header);
    std::memcpy(out, marker_magic, sizeof(marker_magic));
    foundation::store_le64(out + 4, metadata.generation);
    foundation::store_le32(out + 12, metadata.payload_crc);
    foundation::store_le32(out + 16, foundation::load_le32(header + 68));
    foundation::store_le32(out + 20, foundation::crc32(out, 20));
}

BatchCheck read_snapshot_marker(const uint8_t *bytes, uint32_t size,
                                const SnapshotMetadata &metadata) {
    uint8_t expected[snapshot_marker_size]{};
    write_snapshot_marker(metadata, expected);
    return size == snapshot_marker_size && std::memcmp(bytes, expected, size) == 0
               ? BatchCheck::ok
               : BatchCheck::corrupt;
}

} // namespace ayni::durability
