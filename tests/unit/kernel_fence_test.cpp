// C-COMMIT-008/010/011: retries keep batch identity and isolate physical
// writers before replacing the one mutable work model.
#include "foundation/little_endian.h"
#include "work_node.h"

#include <ayni/hosts/sim.h>
#include <gtest/gtest.h>

namespace {
using namespace ayni::test;
using ayni::hosts::sim::StorageFault;
using ayni::hosts::sim::StoragePort;

ayni_step_report_v1 tick(WorkNode &work) {
    const auto report = work.node().step();
    work.node().sim().complete_storage();
    if (report.more_ready == 0U && work.node().sim().pending_storage() == 0U &&
        report.next_deadline_us != UINT64_MAX &&
        report.next_deadline_us > work.node().sim().now_us()) {
        work.node().sim().advance_us(report.next_deadline_us - work.node().sim().now_us());
    }
    return report;
}

void settle(WorkNode &work) {
    for (int round = 0; round < 1000; ++round) {
        tick(work);
        // White-box: a stable RUNNING writer owns no pending completions.
        if (work.node().lifecycle() == AYNI_LIFECYCLE_RESTRICTED ||
            (work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING && work.kernel()->log.idle() &&
             work.kernel()->completions.in_flight() == 0U &&
             !work.kernel()->maintenance.active())) {
            return;
        }
    }
    FAIL() << "storage never settles";
}

ayni_goal_create_v1 folder(WorkNode &work) {
    return WorkNode::create_body(work.instance(agent_a), 0, "folder");
}

TEST(KernelFence, normal_append_in_flight_keeps_room_for_later_transitions) {
    WorkNode work;
    work.run();
    ASSERT_EQ(work.create(folder(work)).status, AYNI_OK);
    // White-box: starts the initial append without applying work decisions.
    work.node().sim().advance_us(10000);
    work.node().step();
    ASSERT_EQ(work.node().sim().pending_storage(), 1U);
    EXPECT_EQ(work.create(folder(work)).status, AYNI_OK);
    settle(work);
}

TEST(KernelFence, persisted_unknown_retries_one_batch_and_freezes_new_commands) {
    WorkNode work;
    work.run();
    const auto body = folder(work);
    auto command = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    command.command_id = 41;
    const auto accepted = work.submit(command);
    ASSERT_EQ(accepted.status, AYNI_OK);
    const auto before = work.node().sim().log().size();
    work.node().sim().inject_fault(StoragePort::append, StorageFault::unknown_persisted);
    work.node().sim().advance_us(10000);
    work.node().step();
    work.node().sim().complete_storage();
    const uint64_t failure_time = work.node().sim().now_us();
    const auto report = work.node().step(1);
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.submit(command).status, AYNI_OK);
    const auto blocked = work.create(body);
    EXPECT_EQ(blocked.status, AYNI_BUSY);
    EXPECT_EQ(blocked.error.reason, AYNI_REASON_STORAGE_BACKLOG);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_GE(report.next_deadline_us, failure_time + 10000U);
    settle(work);
    EXPECT_EQ(work.node().sim().log().size(), before + 1U);
    EXPECT_EQ(work.goal(accepted.result.object.id).revision, 1U);
    work.node().reboot();
    work.node().recover();
    EXPECT_EQ(work.goal(accepted.result.object.id).revision, 1U);
}

TEST(KernelFence, exhausted_retries_reload_once_and_close_old_sessions) {
    WorkNode work;
    work.run();
    const uint64_t old_epoch = work.node().info().boot_epoch;
    const auto body = folder(work);
    const auto accepted = work.create(body);
    ASSERT_EQ(accepted.status, AYNI_OK);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::unknown_deferred);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    settle(work);
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.node().info().boot_epoch, old_epoch + 1U);
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_goal(work.kernel(), accepted.result.object.id, &info), AYNI_NOT_FOUND);
    EXPECT_EQ(ayni_close_session(work.kernel(), work.session()), AYNI_NOT_FOUND);
    const auto log_size = work.node().sim().log().size();
    EXPECT_EQ(work.node().sim().complete_late_storage(), 0U);
    EXPECT_EQ(work.node().sim().log().size(), log_size);
    work.reopen_session();
    EXPECT_EQ(work.create(folder(work)).status, AYNI_OK);
}

TEST(KernelFence, unavailable_isolation_preserves_diagnostics_without_new_boot) {
    WorkNode work;
    work.run();
    const auto accepted = work.create(folder(work));
    ASSERT_EQ(accepted.status, AYNI_OK);
    for (int attempt = 0; attempt < 3; ++attempt) {
        work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    }
    work.node().sim().inject_fault(StoragePort::fence, StorageFault::unavailable);
    const auto before = work.node().sim().log().size();
    settle(work);
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(work.node().sim().log().size(), before);
    EXPECT_EQ(work.goal(accepted.result.object.id).revision, 1U);
}

TEST(KernelFence, shutdown_during_retry_never_starts_recovery) {
    WorkNode work;
    work.run();
    ASSERT_EQ(work.create(folder(work)).status, AYNI_OK);
    for (int attempt = 0; attempt < 3; ++attempt) {
        work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    }
    work.node().sim().advance_us(10000);
    work.node().step();
    work.node().sim().complete_storage();
    work.node().step(1);
    const auto before = work.node().sim().log().size();
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    for (int round = 0; round < 100 && work.node().lifecycle() != AYNI_LIFECYCLE_STOPPED; ++round) {
        tick(work);
    }
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_EQ(work.node().sim().log().size(), before);
}
} // namespace

namespace {
std::vector<uint8_t> encoded_fence(uint32_t writer_epoch, uint64_t seq) {
    std::vector<uint8_t> bytes(128);
    uint8_t body[4]{};
    ayni::foundation::store_le32(body, writer_epoch);
    ayni::durability::BatchWriter writer(bytes.data(), bytes.size(), {writer_epoch, seq, seq});
    EXPECT_TRUE(writer.add_record(
        {ayni::durability::writer_fence_record_type, 1, ayni::durability::record_required, 4},
        body));
    bytes.resize(writer.finish());
    return bytes;
}
std::vector<uint8_t> encoded_empty(uint32_t epoch, uint64_t seq, uint64_t transition = 0) {
    std::vector<uint8_t> bytes(128);
    ayni::durability::BatchWriter writer(bytes.data(), bytes.size(),
                                         {epoch, seq, transition == 0U ? seq : transition});
    bytes.resize(writer.finish());
    return bytes;
}
ayni::durability::RecordUse unused_record(void *, const ayni::durability::BatchIdentity &,
                                          const ayni::durability::RecordHeader &, const uint8_t *) {
    return ayni::durability::RecordUse::unknown;
}
TEST(WriterFenceScan, stale_physical_batch_never_moves_the_logical_cursor) {
    ayni::durability::PrefixScan prefix(1);
    for (const auto &bytes : {encoded_empty(1, 1), encoded_fence(2, 2), encoded_empty(1, 90, 500),
                              encoded_empty(2, 3)}) {
        ASSERT_EQ(prefix.scan(bytes.data(), bytes.size(), unused_record, nullptr),
                  ayni::durability::BatchCheck::ok);
    }
    EXPECT_EQ(prefix.next_commit_seq(), 4U);
    EXPECT_EQ(prefix.max_transition_id(), 3U);
    EXPECT_EQ(prefix.max_writer_epoch(), 2U);
}
TEST(WriterFenceScan, fence_must_advance_the_writer_and_match_its_header) {
    for (const uint32_t epoch : {0U, 1U}) {
        ayni::durability::PrefixScan prefix(1);
        const auto first = encoded_empty(1, 1);
        ASSERT_EQ(prefix.scan(first.data(), first.size(), unused_record, nullptr),
                  ayni::durability::BatchCheck::ok);
        const auto invalid = encoded_fence(epoch, 2);
        EXPECT_EQ(prefix.scan(invalid.data(), invalid.size(), unused_record, nullptr),
                  ayni::durability::BatchCheck::corrupt);
    }
}
} // namespace

namespace {
struct LegacyImage {
    ayni::hosts::sim::StorageImage image;
    uint64_t goal_id = 0;
};

LegacyImage legacy_image() {
    WorkNode source;
    source.run();
    const auto accepted = source.create(folder(source));
    EXPECT_EQ(accepted.status, AYNI_OK);
    settle(source);
    LegacyImage out{};
    out.goal_id = accepted.result.object.id;
    uint64_t seq = 0;
    for (const auto &stored : source.node().sim().log()) {
        ayni::durability::BatchView batch{};
        EXPECT_EQ(ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch),
                  ayni::durability::BatchCheck::ok);
        ayni::durability::RecordReader records(batch);
        ayni::durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        std::vector<std::pair<ayni::durability::RecordHeader, std::vector<uint8_t>>> kept;
        bool fence = false;
        while (records.next(header, body) == ayni::durability::RecordReader::Next::record) {
            fence = fence || header.type == ayni::durability::writer_fence_record_type;
            kept.push_back({header, std::vector<uint8_t>(body, body + header.length)});
        }
        if (fence) {
            continue;
        }
        ++seq;
        std::vector<uint8_t> bytes(65536);
        ayni::durability::BatchWriter writer(bytes.data(), bytes.size(),
                                             {batch.identity.writer_epoch, seq, seq}, 0);
        for (const auto &record : kept) {
            EXPECT_TRUE(writer.add_record(record.first, record.second.data()));
        }
        bytes.resize(writer.finish());
        out.image.log.push_back({seq, std::move(bytes)});
    }
    return out;
}

// A schema-0 reader knows only the implemented record types 1..9. This is
// a rollback-reader simulation, not an execution of an old firmware image.
bool legacy_reader_accepts(const std::vector<ayni::hosts::sim::StoredBatch> &log) {
    for (const auto &stored : log) {
        ayni::durability::BatchView batch{};
        if (ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch) !=
                ayni::durability::BatchCheck::ok ||
            batch.schema != 0U) {
            return false;
        }
        ayni::durability::RecordReader records(batch);
        ayni::durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        while (records.next(header, body) == ayni::durability::RecordReader::Next::record) {
            if (header.type > 9U && (header.flags & ayni::durability::record_required) != 0U) {
                return false;
            }
        }
    }
    return true;
}

TEST(StorageMigration, schema_zero_work_migrates_and_the_old_reader_rejects_the_switch) {
    const auto legacy = legacy_image();
    ASSERT_TRUE(legacy_reader_accepts(legacy.image.log));
    WorkNode work;
    work.node().sim().durable_storage() = legacy.image;
    work.node().create_and_start();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.goal(legacy.goal_id).revision, 1U);
    EXPECT_EQ(work.node().info().boot_epoch, 2U);
    EXPECT_FALSE(legacy_reader_accepts(work.node().sim().log()));
    ASSERT_FALSE(work.node().sim().durable_storage().snapshots[0].empty());
    work.node().reboot();
    work.node().recover();
    EXPECT_EQ(work.goal(legacy.goal_id).revision, 1U);
}

TEST(StorageMigration, every_early_request_can_lose_power_without_losing_the_legacy_state) {
    const auto legacy = legacy_image();
    WorkNode baseline;
    baseline.node().sim().durable_storage() = legacy.image;
    baseline.node().create_and_start();
    for (int round = 0; round < 1000 && baseline.node().lifecycle() == AYNI_LIFECYCLE_RECOVERING;
         ++round) {
        tick(baseline);
    }
    ASSERT_EQ(baseline.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const std::size_t request_count = baseline.node().sim().storage_attempts().size();
    ASSERT_GT(request_count, 10U);
    // Includes discovery/replay, isolation, candidate writes/readback/marker,
    // activation and trim, then the selected snapshot and new boot path.
    for (std::size_t cut = 1; cut <= request_count; ++cut) {
        SCOPED_TRACE(cut);
        WorkNode work;
        work.node().sim().durable_storage() = legacy.image;
        work.node().create_and_start();
        bool reached = false;
        for (int round = 0; round < 1000; ++round) {
            const auto report = work.node().step(1);
            if (work.node().sim().storage_attempts().size() >= cut &&
                work.node().sim().pending_storage() != 0U) {
                reached = true;
                break;
            }
            work.node().sim().complete_storage();
            if (report.more_ready == 0U && work.node().sim().pending_storage() == 0U &&
                report.next_deadline_us != UINT64_MAX &&
                report.next_deadline_us > work.node().sim().now_us()) {
                work.node().sim().advance_us(report.next_deadline_us - work.node().sim().now_us());
            }
            if (work.node().lifecycle() != AYNI_LIFECYCLE_RECOVERING) {
                break;
            }
        }
        ASSERT_TRUE(reached);
        if (legacy_reader_accepts(work.node().sim().log())) {
            ASSERT_EQ(work.node().sim().log().size(), legacy.image.log.size());
            for (std::size_t index = 0; index < legacy.image.log.size(); ++index) {
                EXPECT_EQ(work.node().sim().log()[index].bytes, legacy.image.log[index].bytes);
            }
        }
        work.node().power_cut_and_start(ayni::hosts::sim::CutAppend::lost);
        work.node().recover();
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        EXPECT_EQ(work.goal(legacy.goal_id).revision, 1U);
    }
}
} // namespace

namespace {
TEST(KernelFence, incomplete_replayed_transition_never_installs_a_writer_or_appends_metadata) {
    WorkNode source;
    source.run();
    ASSERT_EQ(source.create(folder(source)).status, AYNI_OK);
    settle(source);
    auto image = source.node().sim().durable_storage();
    auto &stored = image.log.back();
    ayni::durability::BatchView batch{};
    ASSERT_EQ(ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch),
              ayni::durability::BatchCheck::ok);
    std::vector<uint8_t> rewritten(stored.bytes.size());
    ayni::durability::BatchWriter writer(rewritten.data(), rewritten.size(), batch.identity);
    ayni::durability::RecordReader reader(batch);
    ayni::durability::RecordHeader header{};
    const uint8_t *body = nullptr;
    while (reader.next(header, body) == ayni::durability::RecordReader::Next::record) {
        if (header.type != 4U) {
            ASSERT_TRUE(writer.add_record(header, body));
        }
    }
    rewritten.resize(writer.finish());
    stored.bytes = std::move(rewritten);
    const auto old_epoch = image.writer_epoch;
    WorkNode recovered;
    recovered.node().sim().durable_storage() = image;
    recovered.node().create_and_start();
    recovered.node().recover();
    EXPECT_EQ(recovered.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(recovered.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
    EXPECT_EQ(recovered.node().sim().writer_epoch(), old_epoch);
    ASSERT_EQ(recovered.node().sim().log().size(), image.log.size());
    EXPECT_EQ(recovered.node().sim().log().back().bytes, image.log.back().bytes);
}
} // namespace
