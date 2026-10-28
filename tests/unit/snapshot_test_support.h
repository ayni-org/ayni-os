// White-box helpers for C-STORE-007/009: use the real snapshot writer/reader and
// record envelope codec; individual tests own their domain-specific oracles.
#ifndef AYNI_TESTS_UNIT_SNAPSHOT_TEST_SUPPORT_H
#define AYNI_TESTS_UNIT_SNAPSHOT_TEST_SUPPORT_H

#include "kernel/service/persistence/work_snapshot.h"
#include "kernel_node.h"

namespace ayni::test {
struct SnapshotRecord {
    durability::RecordHeader header;
    std::vector<uint8_t> body;
};
struct ModelSnapshot {
    durability::SnapshotMetadata metadata{};
    ayni_config_v1 config{};
    std::vector<SnapshotRecord> records;
};

inline ModelSnapshot snapshot_model(Node &source) {
    auto &model = kernel::KernelTestProbe::work(source.kernel());
    const auto layout = kernel::measure_snapshot(model);
    ModelSnapshot result{};
    result.config = source.config();
    result.metadata.generation = 1;
    result.metadata.covers_commit_seq = model.transitions.latest().commit_seq;
    result.metadata.max_transition_id = model.transitions.latest().transition_id;
    result.metadata.boot_epoch = source.info().boot_epoch;
    result.metadata.writer_epoch = 1;
    result.metadata.payload_length = layout.payload_length;
    result.metadata.record_count = layout.record_count;
    kernel::WorkSnapshotWriter writer(model);
    std::vector<uint8_t> page(test_storage_buffer_bytes);
    for (;;) {
        uint32_t size = 0;
        const auto next = writer.next(page.data(), static_cast<uint32_t>(page.size()), size);
        EXPECT_NE(next, kernel::WorkSnapshotWriter::Page::no_room);
        if (next != kernel::WorkSnapshotWriter::Page::page) {
            break;
        }
        uint32_t at = 0;
        while (at < size) {
            const auto header = durability::read_record_header(page.data() + at);
            at += static_cast<uint32_t>(durability::record_header_size);
            EXPECT_LE(header.length, size - at);
            if (header.length > size - at) {
                return result;
            }
            result.records.push_back(
                {header, {page.data() + at, page.data() + at + header.length}});
            at += header.length;
        }
    }
    EXPECT_EQ(result.records.size(), layout.record_count);
    return result;
}

// A failed recovery model must be discarded; no test exposes it as a live Kernel.
// Target configuration may be smaller to verify no_room separately from corruption.
inline durability::BatchCheck restore_model(const ModelSnapshot &snapshot, Node &target) {
    target.create();
    auto metadata = snapshot.metadata;
    metadata.record_count = static_cast<uint32_t>(snapshot.records.size());
    kernel::WorkSnapshotReader reader(kernel::KernelTestProbe::work(target.kernel()), metadata);
    for (const auto &record : snapshot.records) {
        const auto status = reader.apply(record.header, record.body.data());
        if (status != durability::BatchCheck::ok) {
            return status;
        }
    }
    return reader.complete();
}

inline durability::BatchCheck restore_model(const ModelSnapshot &snapshot) {
    Node target;
    target.config() = snapshot.config;
    return restore_model(snapshot, target);
}
} // namespace ayni::test
#endif
