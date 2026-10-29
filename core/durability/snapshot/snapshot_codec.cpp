#include "durability/snapshot/snapshot_codec.h"

#include "foundation/crc32.h"
#include "foundation/kernel_id.h"
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

constexpr uint8_t magic[4] = {'A', 'Y', 'S', 'N'};
constexpr uint8_t marker_magic[4] = {'A', 'Y', 'S', 'C'};

// Header field offsets.
constexpr uint32_t schema_at = 4;
constexpr uint32_t header_length_at = 6;
constexpr uint32_t generation_at = 8;
constexpr uint32_t covers_commit_seq_at = 16;
constexpr uint32_t max_transition_id_at = 24;
constexpr uint32_t boot_epoch_at = 32;
constexpr uint32_t writer_epoch_at = 40;
constexpr uint32_t flags_at = 44;
constexpr uint32_t payload_length_at = 48;
constexpr uint32_t record_count_at = 56;
constexpr uint32_t payload_crc_at = 60;
constexpr uint32_t reserved_at = 64;
constexpr uint32_t header_crc_at = 68;

// Marker field offsets.
constexpr uint32_t marker_generation_at = 4;
constexpr uint32_t marker_payload_crc_at = 12;
constexpr uint32_t marker_header_crc_at = 16;
constexpr uint32_t marker_crc_at = 20;

// SNAPSHOT_COMMIT body offsets.
constexpr uint32_t activation_slot_at = 0;
constexpr uint32_t activation_payload_crc_at = 4;
constexpr uint32_t activation_generation_at = 8;
constexpr uint32_t activation_covers_commit_seq_at = 16;
constexpr uint32_t activation_max_transition_id_at = 24;

} // namespace

uint32_t snapshot_crc32(const uint8_t *bytes, uint32_t size, uint32_t previous) {
    return crc32(bytes, size, previous);
}

void write_snapshot_header(const SnapshotMetadata &metadata, uint8_t *out) {
    std::memset(out, 0, snapshot_header_size);
    std::memcpy(out, magic, sizeof(magic));
    store_le16(out + schema_at, snapshot_schema);
    store_le16(out + header_length_at, snapshot_header_size);
    store_le64(out + generation_at, metadata.generation);
    store_le64(out + covers_commit_seq_at, metadata.covers_commit_seq);
    store_le64(out + max_transition_id_at, metadata.max_transition_id);
    store_le64(out + boot_epoch_at, metadata.boot_epoch);
    store_le32(out + writer_epoch_at, metadata.writer_epoch);
    store_le64(out + payload_length_at, metadata.payload_length);
    store_le32(out + record_count_at, metadata.record_count);
    store_le32(out + payload_crc_at, metadata.payload_crc);
    store_le32(out + header_crc_at, crc32(out, header_crc_at));
}

// The CRC is checked before the schema, so a damaged schema field reads as
// corruption, not as a newer format.
BatchCheck read_snapshot_header(const uint8_t *bytes, uint32_t size, SnapshotMetadata &out) {
    if (size != snapshot_header_size || std::memcmp(bytes, magic, sizeof(magic)) != 0 ||
        load_le16(bytes + header_length_at) != snapshot_header_size ||
        load_le32(bytes + header_crc_at) != crc32(bytes, header_crc_at)) {
        return BatchCheck::corrupt;
    }
    if (load_le16(bytes + schema_at) != snapshot_schema || load_le32(bytes + flags_at) != 0U ||
        load_le32(bytes + reserved_at) != 0U) {
        return BatchCheck::unsupported;
    }
    SnapshotMetadata metadata{};
    metadata.schema = load_le16(bytes + schema_at);
    metadata.generation = load_le64(bytes + generation_at);
    metadata.covers_commit_seq = load_le64(bytes + covers_commit_seq_at);
    metadata.max_transition_id = load_le64(bytes + max_transition_id_at);
    metadata.boot_epoch = load_le64(bytes + boot_epoch_at);
    metadata.writer_epoch = load_le32(bytes + writer_epoch_at);
    metadata.payload_length = load_le64(bytes + payload_length_at);
    metadata.record_count = load_le32(bytes + record_count_at);
    metadata.payload_crc = load_le32(bytes + payload_crc_at);
    if (metadata.generation == 0U || metadata.writer_epoch == 0U ||
        metadata.boot_epoch > foundation::max_boot_epoch ||
        metadata.max_transition_id > transition_id_max ||
        metadata.covers_commit_seq > metadata.max_transition_id ||
        (metadata.record_count == 0U) != (metadata.payload_length == 0U) ||
        metadata.payload_length < uint64_t{metadata.record_count} * record_header_size ||
        // Only an empty snapshot, which no batch precedes, covers commit 0.
        (metadata.covers_commit_seq == 0U &&
         (metadata.max_transition_id != 0U || metadata.boot_epoch != 0U ||
          metadata.record_count != 0U))) {
        return BatchCheck::corrupt;
    }
    out = metadata;
    return BatchCheck::ok;
}

// The marker repeats the header CRC, so it binds every header field.
void write_snapshot_marker(const SnapshotMetadata &metadata, uint8_t *out) {
    uint8_t header[snapshot_header_size]{};
    write_snapshot_header(metadata, header);
    std::memcpy(out, marker_magic, sizeof(marker_magic));
    store_le64(out + marker_generation_at, metadata.generation);
    store_le32(out + marker_payload_crc_at, metadata.payload_crc);
    store_le32(out + marker_header_crc_at, load_le32(header + header_crc_at));
    store_le32(out + marker_crc_at, crc32(out, marker_crc_at));
}

BatchCheck read_snapshot_marker(const uint8_t *bytes, uint32_t size,
                                const SnapshotMetadata &metadata) {
    uint8_t expected[snapshot_marker_size]{};
    write_snapshot_marker(metadata, expected);
    return size == snapshot_marker_size && std::memcmp(bytes, expected, size) == 0
               ? BatchCheck::ok
               : BatchCheck::corrupt;
}

void write_snapshot_activation(const SnapshotActivation &activation, uint8_t *body) {
    store_le32(body + activation_slot_at, activation.slot);
    store_le32(body + activation_payload_crc_at, activation.payload_crc);
    store_le64(body + activation_generation_at, activation.generation);
    store_le64(body + activation_covers_commit_seq_at, activation.covers_commit_seq);
    store_le64(body + activation_max_transition_id_at, activation.max_transition_id);
}

bool read_snapshot_activation(const uint8_t *body, uint32_t size, SnapshotActivation &out) {
    if (size != snapshot_commit_record_length) {
        return false;
    }
    out.slot = load_le32(body + activation_slot_at);
    out.payload_crc = load_le32(body + activation_payload_crc_at);
    out.generation = load_le64(body + activation_generation_at);
    out.covers_commit_seq = load_le64(body + activation_covers_commit_seq_at);
    out.max_transition_id = load_le64(body + activation_max_transition_id_at);
    return out.slot < 2U && out.generation != 0U && out.max_transition_id <= transition_id_max &&
           out.covers_commit_seq <= out.max_transition_id;
}

bool activation_matches(const SnapshotActivation &activation, const SnapshotMetadata &metadata) {
    return activation.generation == metadata.generation &&
           activation.covers_commit_seq == metadata.covers_commit_seq &&
           activation.max_transition_id == metadata.max_transition_id &&
           activation.payload_crc == metadata.payload_crc;
}

} // namespace ayni::durability
