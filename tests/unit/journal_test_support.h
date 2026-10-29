// White-box CRC-valid semantic faults (C-STORE-009): framing stays with the
// production batch codec; domain tests change only complete owned record bodies.
#ifndef AYNI_TESTS_UNIT_JOURNAL_TEST_SUPPORT_H
#define AYNI_TESTS_UNIT_JOURNAL_TEST_SUPPORT_H
#include "durability/batch/batch_codec.h"
#include <ayni/hosts/sim.h>
#include <gtest/gtest.h>
#include <vector>

namespace ayni::test {
struct JournalRecord {
    durability::RecordHeader header;
    std::vector<uint8_t> body;
};
struct JournalBatch {
    durability::BatchIdentity identity;
    uint16_t schema;
    std::vector<JournalRecord> records;
};
inline JournalBatch read_stored_batch(const hosts::sim::StoredBatch &stored) {
    durability::BatchView view{};
    EXPECT_FALSE(stored.torn);
    EXPECT_EQ(durability::read_batch(stored.bytes.data(), stored.bytes.size(), view),
              durability::BatchCheck::ok);
    JournalBatch result{view.identity, view.schema, {}};
    durability::RecordReader reader(view);
    durability::RecordHeader header{};
    const uint8_t *body = nullptr;
    for (;;) {
        const auto next = reader.next(header, body);
        if (next == durability::RecordReader::Next::end) {
            break;
        }
        EXPECT_EQ(next, durability::RecordReader::Next::record);
        if (next != durability::RecordReader::Next::record) {
            break;
        }
        result.records.push_back({header, {body, body + header.length}});
    }
    return result;
}
inline hosts::sim::StoredBatch write_stored_batch(const JournalBatch &batch) {
    std::size_t body_size = 0;
    for (const auto &record : batch.records) {
        body_size += record.body.size();
    }
    hosts::sim::StoredBatch stored{};
    stored.commit_seq = batch.identity.commit_seq;
    stored.bytes.resize(durability::batch_size(body_size, batch.records.size()));
    durability::BatchWriter writer(stored.bytes.data(), stored.bytes.size(), batch.identity);
    for (const auto &record : batch.records) {
        EXPECT_EQ(record.header.length, record.body.size());
        EXPECT_TRUE(writer.add_record(record.header, record.body.data()));
    }
    EXPECT_EQ(writer.finish(), stored.bytes.size());
    durability::BatchView checked{};
    EXPECT_EQ(durability::read_batch(stored.bytes.data(), stored.bytes.size(), checked),
              durability::BatchCheck::ok);
    return stored;
}
} // namespace ayni::test
#endif
