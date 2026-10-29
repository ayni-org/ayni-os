// Log prefix checks owned by durability (C-STORE-003, C-STORE-004,
// C-STORE-009): commit_seq continuity across reads, record flags, unknown
// records and what a record owner reports.

#include "durability/batch/batch_codec.h"
#include "durability/batch/prefix_scan.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using ayni::durability::BatchCheck;
using ayni::durability::BatchIdentity;
using ayni::durability::BatchWriter;
using ayni::durability::PrefixScan;
using ayni::durability::record_required;
using ayni::durability::RecordHeader;
using ayni::durability::RecordUse;

constexpr uint16_t known_type = 5;

// Appends one batch of empty records to log.
void append(std::vector<uint8_t> &log, const BatchIdentity &identity,
            const std::vector<RecordHeader> &records) {
    std::vector<uint8_t> bytes(512);
    BatchWriter writer(bytes.data(), bytes.size(), identity);
    for (const RecordHeader &record : records) {
        EXPECT_TRUE(writer.add_record(record, nullptr));
    }
    bytes.resize(writer.finish());
    log.insert(log.end(), bytes.begin(), bytes.end());
}

// Knows known_type version 1; counts what it is shown and notes the batch of
// the last record. A record with version 1 but type known_type + 1 stands
// for a body the owner rejects, and one of type known_type + 2 for one it
// has no room for.
struct Owner {
    static RecordUse handle(void *context, const BatchIdentity &batch, const RecordHeader &header,
                            const uint8_t * /*body*/) {
        Owner &self = *static_cast<Owner *>(context);
        ++self.seen;
        self.last_batch = batch;
        if (header.type == known_type + 1 && header.version == 1) {
            return RecordUse::corrupt;
        }
        if (header.type == known_type + 2 && header.version == 1) {
            return RecordUse::no_room;
        }
        return header.type == known_type && header.version == 1 ? RecordUse::applied
                                                                : RecordUse::unknown;
    }
    int seen = 0;
    BatchIdentity last_batch{};
};

BatchCheck scan(PrefixScan &prefix, const std::vector<uint8_t> &log, Owner &owner) {
    return prefix.scan(log.data(), log.size(), &Owner::handle, &owner);
}

TEST(PrefixScan, continues_across_reads_and_tracks_identity) {
    std::vector<uint8_t> first;
    append(first, {2, 1, 10}, {{known_type, 1, record_required, 0}});
    append(first, {3, 2, 9}, {{known_type, 1, 0, 0}, {known_type, 1, 0, 0}});
    std::vector<uint8_t> second;
    append(second, {3, 3, 4}, {});

    PrefixScan prefix(1);
    Owner owner;
    ASSERT_EQ(scan(prefix, first, owner), BatchCheck::ok);
    ASSERT_EQ(scan(prefix, second, owner), BatchCheck::ok);
    EXPECT_EQ(owner.seen, 3);
    EXPECT_EQ(prefix.next_commit_seq(), 4U);
    EXPECT_EQ(prefix.max_transition_id(), 10U);
    EXPECT_EQ(prefix.max_writer_epoch(), 3U);
}

// Replay stamps what it restores with the batch a record came in.
TEST(PrefixScan, each_record_comes_with_its_batch) {
    std::vector<uint8_t> log;
    append(log, {2, 1, 10}, {{known_type, 1, 0, 0}});
    append(log, {2, 2, 11}, {{known_type, 1, 0, 0}});
    PrefixScan prefix(1);
    Owner owner;
    ASSERT_EQ(scan(prefix, log, owner), BatchCheck::ok);
    EXPECT_EQ(owner.last_batch.writer_epoch, 2U);
    EXPECT_EQ(owner.last_batch.commit_seq, 2U);
    EXPECT_EQ(owner.last_batch.transition_id, 11U);
}

TEST(PrefixScan, record_without_room_stops_the_scan) {
    std::vector<uint8_t> log;
    append(log, {1, 1, 1}, {{known_type + 2, 1, record_required, 0}, {known_type, 1, 0, 0}});
    append(log, {1, 2, 2}, {{known_type, 1, 0, 0}});
    PrefixScan prefix(1);
    Owner owner;
    EXPECT_EQ(scan(prefix, log, owner), BatchCheck::no_room);
    EXPECT_EQ(owner.seen, 1);
    EXPECT_EQ(prefix.next_commit_seq(), 1U);
}

TEST(PrefixScan, empty_prefix_leaves_the_start_unchanged) {
    PrefixScan prefix(7);
    Owner owner;
    EXPECT_EQ(prefix.scan(nullptr, 0, &Owner::handle, &owner), BatchCheck::ok);
    EXPECT_EQ(prefix.next_commit_seq(), 7U);
    EXPECT_EQ(prefix.max_transition_id(), 0U);
    EXPECT_EQ(prefix.max_writer_epoch(), 0U);
}

// C-STORE-003: the Host returns whole batches only, so a gap, a repeat or a
// wrong start is damage in the committed region, not the end of the prefix.
TEST(PrefixScan, commit_seq_must_continue_exactly) {
    for (const uint64_t second_seq : {uint64_t{1}, uint64_t{3}}) {
        std::vector<uint8_t> log;
        append(log, {1, 1, 1}, {});
        append(log, {1, second_seq, 2}, {});
        PrefixScan prefix(1);
        Owner owner;
        EXPECT_EQ(scan(prefix, log, owner), BatchCheck::corrupt) << second_seq;
    }
    std::vector<uint8_t> log;
    append(log, {1, 2, 1}, {});
    PrefixScan prefix(1);
    Owner owner;
    EXPECT_EQ(scan(prefix, log, owner), BatchCheck::corrupt);
}

// Until fence records exist, a batch from an older writer after a newer one
// has no explanation (C-STORE-005).
TEST(PrefixScan, older_writer_after_a_newer_one_is_corrupt) {
    std::vector<uint8_t> log;
    append(log, {2, 1, 1}, {});
    append(log, {1, 2, 2}, {});
    PrefixScan prefix(1);
    Owner owner;
    EXPECT_EQ(scan(prefix, log, owner), BatchCheck::corrupt);

    std::vector<uint8_t> newer;
    append(newer, {1, 1, 1}, {});
    append(newer, {2, 2, 2}, {});
    PrefixScan growing(1);
    EXPECT_EQ(scan(growing, newer, owner), BatchCheck::ok);
}

// C-STORE-009: damage stops the scan; later batches are not handed out.
TEST(PrefixScan, damaged_batch_stops_the_scan) {
    std::vector<uint8_t> log;
    append(log, {1, 1, 1}, {{known_type, 1, 0, 0}});
    const std::size_t second_at = log.size();
    append(log, {1, 2, 2}, {{known_type, 1, 0, 0}});
    append(log, {1, 3, 3}, {{known_type, 1, 0, 0}});
    log[second_at + 20] ^= 0x01U;
    PrefixScan prefix(1);
    Owner owner;
    EXPECT_EQ(scan(prefix, log, owner), BatchCheck::corrupt);
    EXPECT_EQ(owner.seen, 1);
}

// C-STORE-004/ADR-0082: unknown types, versions and flag bits stop the scan
// regardless of REQUIRED; a known type without REQUIRED is corrupt.
TEST(PrefixScan, unknown_records_are_rejected_regardless_of_the_required_flag) {
    const struct {
        RecordHeader record;
        BatchCheck expected;
    } cases[] = {
        {{99, 1, 0, 0}, BatchCheck::unsupported},
        {{known_type, 2, 0, 0}, BatchCheck::unsupported},
        {{99, 1, record_required, 0}, BatchCheck::unsupported},
        {{known_type, 2, record_required, 0}, BatchCheck::unsupported},
        {{known_type, 1, 0x4U, 0}, BatchCheck::unsupported},
        {{known_type + 1, 1, 0, 0}, BatchCheck::corrupt},
    };
    for (const auto &item : cases) {
        std::vector<uint8_t> log;
        append(log, {1, 1, 1}, {item.record});
        PrefixScan prefix(1);
        Owner owner;
        EXPECT_EQ(scan(prefix, log, owner), item.expected)
            << item.record.type << "/" << item.record.version << "/" << item.record.flags;
    }
}

} // namespace
