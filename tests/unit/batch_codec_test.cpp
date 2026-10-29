// Commit batch encoding (contracts/storage/records.md): round trip, every
// kind of damage detected, header extension and unknown versions.

#include "durability/batch/batch_codec.h"
#include "foundation/crc32.h"
#include "foundation/little_endian.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using ayni::durability::batch_header_size;
using ayni::durability::BatchCheck;
using ayni::durability::BatchIdentity;
using ayni::durability::BatchView;
using ayni::durability::BatchWriter;
using ayni::durability::commit_marker_size;
using ayni::durability::read_batch;
using ayni::durability::RecordHeader;
using ayni::durability::RecordReader;
using ayni::foundation::crc32;
using ayni::foundation::load_le16;
using ayni::foundation::load_le32;
using ayni::foundation::store_le16;
using ayni::foundation::store_le32;
using ayni::foundation::store_le64;

constexpr BatchIdentity identity{3, 0x1'0000'0007ULL, 41};
constexpr std::array<uint8_t, 5> first_body{1, 2, 3, 4, 5};
constexpr std::array<uint8_t, 3> second_body{9, 8, 7};

std::vector<uint8_t> sample_batch() {
    std::vector<uint8_t> bytes(256);
    BatchWriter writer(bytes.data(), bytes.size(), identity);
    EXPECT_TRUE(writer.add_record({7, 1, ayni::durability::record_required, 5}, first_body.data()));
    EXPECT_TRUE(writer.add_record({8, 2, 0, 3}, second_body.data()));
    bytes.resize(writer.finish());
    return bytes;
}

// Recomputes both checksums and the commit marker after a test edits fields,
// so that only the edited field is wrong.
void reseal(std::vector<uint8_t> &bytes) {
    const std::size_t header_length = load_le16(bytes.data() + 6);
    store_le32(bytes.data() + header_length - 4, crc32(bytes.data(), header_length - 4));
    const uint32_t payload_length = load_le32(bytes.data() + 36);
    uint8_t *marker = bytes.data() + header_length + payload_length;
    std::memcpy(marker + 4, bytes.data() + 16, 4); // commit_seq_low
    store_le32(marker + 8, crc32(bytes.data() + header_length, payload_length));
}

// ADR-0040: a literal prefix pins endian/field offsets without consulting the
// writer. Schema/flags/CRC and semantic identity validation belong to Core.
TEST(BatchCodec, storage_identity_reads_only_the_unvalidated_prefix) {
    const std::array<uint8_t, AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES> prefix{
        'A',  'Y',  'J',  'B',  0xff, 0xff, 0,    0,    0x12, 0x34, 0x56,
        0x78, 0xff, 0xff, 0xff, 0xff, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab,
        0xcd, 0xef, 0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10};
    BatchIdentity found{};
    ASSERT_EQ(ayni_storage_read_batch_identity(prefix.data(), prefix.size(), &found), 1);
    EXPECT_EQ(found.writer_epoch, 0x78563412U);
    EXPECT_EQ(found.commit_seq, 0xefcdab8967452301ULL);
    EXPECT_EQ(found.transition_id, 0x1032547698badcfeULL);
    BatchView batch{};
    EXPECT_EQ(read_batch(prefix.data(), prefix.size(), batch), BatchCheck::corrupt);
    const auto encoded = sample_batch();
    ASSERT_EQ(ayni_storage_read_batch_identity(encoded.data(), encoded.size(), &found), 1);
    EXPECT_EQ(found.writer_epoch, identity.writer_epoch);
    EXPECT_EQ(found.commit_seq, identity.commit_seq);
    EXPECT_EQ(found.transition_id, identity.transition_id);
}

// ADR-0040: non-prefix data remains opaque to the Host, and unsuccessful
// inspection cannot publish partially decoded identity fields.
TEST(BatchCodec, storage_identity_rejects_invalid_spans_without_changing_output) {
    auto bytes = sample_batch();
    BatchIdentity found = identity;
    EXPECT_EQ(ayni_storage_read_batch_identity(nullptr, bytes.size(), &found), 0);
    EXPECT_EQ(ayni_storage_read_batch_identity(bytes.data(), bytes.size(), nullptr), 0);
    for (std::size_t size = 0; size < AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES; ++size) {
        EXPECT_EQ(ayni_storage_read_batch_identity(bytes.data(), size, &found), 0);
        EXPECT_EQ(found.writer_epoch, identity.writer_epoch);
        EXPECT_EQ(found.commit_seq, identity.commit_seq);
        EXPECT_EQ(found.transition_id, identity.transition_id);
    }
    constexpr std::size_t magic_size = 4;
    for (std::size_t at = 0; at < magic_size; ++at) {
        bytes[at] ^= 1;
        EXPECT_EQ(ayni_storage_read_batch_identity(bytes.data(), bytes.size(), &found), 0);
        EXPECT_EQ(found.writer_epoch, identity.writer_epoch);
        EXPECT_EQ(found.commit_seq, identity.commit_seq);
        EXPECT_EQ(found.transition_id, identity.transition_id);
        bytes[at] ^= 1;
    }
}

TEST(Crc32, matches_the_iso_hdlc_check_value) {
    const char *check = "123456789";
    EXPECT_EQ(crc32(reinterpret_cast<const uint8_t *>(check), 9), 0xCBF43926U);
    EXPECT_EQ(crc32(nullptr, 0), 0U);
}

TEST(BatchCodec, round_trip_keeps_identity_and_records) {
    const std::vector<uint8_t> bytes = sample_batch();
    ASSERT_EQ(bytes.size(), ayni::durability::batch_size(8, 2));

    BatchView batch{};
    ASSERT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::ok);
    EXPECT_EQ(batch.identity.writer_epoch, identity.writer_epoch);
    EXPECT_EQ(batch.identity.commit_seq, identity.commit_seq);
    EXPECT_EQ(batch.identity.transition_id, identity.transition_id);
    EXPECT_EQ(batch.record_count, 2U);
    EXPECT_EQ(batch.encoded_size, bytes.size());

    RecordReader reader(batch);
    RecordHeader header{};
    const uint8_t *body = nullptr;
    ASSERT_EQ(reader.next(header, body), RecordReader::Next::record);
    EXPECT_EQ(header.type, 7U);
    EXPECT_EQ(header.version, 1U);
    EXPECT_EQ(header.flags, ayni::durability::record_required);
    ASSERT_EQ(header.length, first_body.size());
    EXPECT_EQ(std::memcmp(body, first_body.data(), first_body.size()), 0);
    ASSERT_EQ(reader.next(header, body), RecordReader::Next::record);
    EXPECT_EQ(header.type, 8U);
    EXPECT_EQ(header.version, 2U);
    ASSERT_EQ(header.length, second_body.size());
    EXPECT_EQ(std::memcmp(body, second_body.data(), second_body.size()), 0);
    EXPECT_EQ(reader.next(header, body), RecordReader::Next::end);
}

// A batch followed by other bytes reads as the batch alone.
TEST(BatchCodec, trailing_bytes_are_not_part_of_the_batch) {
    std::vector<uint8_t> bytes = sample_batch();
    const std::size_t size = bytes.size();
    bytes.resize(size + 10, 0xEE);
    BatchView batch{};
    ASSERT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::ok);
    EXPECT_EQ(batch.encoded_size, size);
}

// C-STORE-001: any changed byte fails a checksum, the magic or the marker.
// The header CRC is checked before schema and flags, so damage there reads
// as corruption, not as a newer format.
TEST(BatchCodec, every_damaged_byte_is_corrupt) {
    const std::vector<uint8_t> intact = sample_batch();
    for (std::size_t index = 0; index < intact.size(); ++index) {
        std::vector<uint8_t> bytes = intact;
        bytes[index] ^= 0x10U;
        BatchView batch{};
        EXPECT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::corrupt) << index;
    }
}

TEST(BatchCodec, every_short_write_is_corrupt) {
    const std::vector<uint8_t> intact = sample_batch();
    for (std::size_t size = 0; size < intact.size(); ++size) {
        BatchView batch{};
        EXPECT_EQ(read_batch(intact.data(), size, batch), BatchCheck::corrupt) << size;
    }
}

TEST(BatchCodec, header_shorter_than_44_bytes_is_corrupt) {
    std::vector<uint8_t> bytes = sample_batch();
    store_le16(bytes.data() + 6, 40);
    BatchView batch{};
    EXPECT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::corrupt);
}

// A later writer appends header fields before header_crc; this reader
// rejects the longer header after checking its CRC (ADR-0082).
TEST(BatchCodec, longer_header_from_a_later_writer_is_unsupported) {
    const std::vector<uint8_t> intact = sample_batch();
    std::vector<uint8_t> bytes(intact.begin(), intact.begin() + batch_header_size - 4);
    bytes.insert(bytes.end(), {0xAB, 0xCD, 0xEF, 0x01});
    bytes.insert(bytes.end(), 4, 0); // header_crc
    bytes.insert(bytes.end(), intact.begin() + batch_header_size, intact.end());
    store_le16(bytes.data() + 6, batch_header_size + 4);
    reseal(bytes);

    BatchView batch{};
    EXPECT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::unsupported);
}

// C-STORE-004: another storage_schema or unknown flag bits are a format this
// reader does not know, distinct from corruption.
TEST(BatchCodec, other_schema_or_flags_are_unsupported) {
    std::vector<uint8_t> bytes = sample_batch();
    store_le16(bytes.data() + 4, ayni::durability::storage_schema + 1U);
    reseal(bytes);
    BatchView batch{};
    EXPECT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::unsupported);

    bytes = sample_batch();
    store_le32(bytes.data() + 12, 0x2U);
    reseal(bytes);
    EXPECT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::unsupported);
}

// The commit marker binds the payload to this batch's commit_seq.
TEST(BatchCodec, commit_marker_of_another_commit_seq_is_corrupt) {
    std::vector<uint8_t> bytes = sample_batch();
    store_le64(bytes.data() + 16, identity.commit_seq + 1U);
    store_le32(bytes.data() + batch_header_size - 4, crc32(bytes.data(), batch_header_size - 4));
    BatchView batch{};
    EXPECT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::corrupt);
}

TEST(BatchCodec, records_must_fill_the_payload_as_counted) {
    std::vector<uint8_t> bytes = sample_batch();
    store_le32(bytes.data() + 32, 3); // one more record than written
    reseal(bytes);
    BatchView batch{};
    ASSERT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::ok);
    RecordReader reader(batch);
    RecordHeader header{};
    const uint8_t *body = nullptr;
    EXPECT_EQ(reader.next(header, body), RecordReader::Next::record);
    EXPECT_EQ(reader.next(header, body), RecordReader::Next::record);
    EXPECT_EQ(reader.next(header, body), RecordReader::Next::corrupt);

    bytes = sample_batch();
    store_le32(bytes.data() + 32, 1); // the second record is left over
    reseal(bytes);
    ASSERT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::ok);
    RecordReader short_count(batch);
    EXPECT_EQ(short_count.next(header, body), RecordReader::Next::record);
    EXPECT_EQ(short_count.next(header, body), RecordReader::Next::corrupt);
}

TEST(BatchCodec, record_overrunning_the_payload_is_corrupt) {
    std::vector<uint8_t> bytes = sample_batch();
    store_le32(bytes.data() + batch_header_size + 8, 0xFFFFFFF0U); // first record's length
    reseal(bytes);
    BatchView batch{};
    ASSERT_EQ(read_batch(bytes.data(), bytes.size(), batch), BatchCheck::ok);
    RecordReader reader(batch);
    RecordHeader header{};
    const uint8_t *body = nullptr;
    EXPECT_EQ(reader.next(header, body), RecordReader::Next::corrupt);
}

// A record is added only with room left for the commit marker, so finish()
// never fails after a successful add.
TEST(BatchWriter, refuses_records_that_leave_no_room_for_the_marker) {
    std::array<uint8_t, batch_header_size + 12 + 5 + commit_marker_size> exact{};
    BatchWriter fits(exact.data(), exact.size(), identity);
    ASSERT_TRUE(fits.add_record({7, 1, 0, 5}, first_body.data()));
    EXPECT_FALSE(fits.add_record({8, 1, 0, 0}, nullptr));
    EXPECT_EQ(fits.finish(), exact.size());

    std::array<uint8_t, batch_header_size + 12 + 5 + commit_marker_size - 1> short_buffer{};
    BatchWriter one_short(short_buffer.data(), short_buffer.size(), identity);
    EXPECT_FALSE(one_short.add_record({7, 1, 0, 5}, first_body.data()));
    EXPECT_EQ(one_short.finish(), batch_header_size + commit_marker_size);

    BatchWriter huge(exact.data(), exact.size(), identity);
    EXPECT_FALSE(huge.add_record({7, 1, 0, 0xFFFFFFFFU}, first_body.data()));

    std::array<uint8_t, batch_header_size + commit_marker_size - 1> tiny{};
    BatchWriter no_room(tiny.data(), tiny.size(), identity);
    EXPECT_FALSE(no_room.add_record({7, 1, 0, 0}, nullptr));
    EXPECT_EQ(no_room.finish(), 0U);
}

} // namespace
