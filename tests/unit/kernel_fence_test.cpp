#include "foundation/crc32.h"
// C-COMMIT-008/010/011: retries keep batch identity and isolate physical
// writers before replacing the one mutable work model.
#include "authority/principal/owner_binding_records.h"
#include "foundation/little_endian.h"
#include "ingress_test_peer.h"
#include "work/goal/goal_authority_records.h"
#include "work/goal/goal_records.h"
#include "work_node.h"

#include <ayni/hosts/sim.h>
#include <gtest/gtest.h>

using ayni::kernel::KernelTestProbe;

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
            (work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING &&
             KernelTestProbe::log(work.kernel()).idle() &&
             KernelTestProbe::completions(work.kernel()).in_flight() == 0U &&
             !KernelTestProbe::maintenance(work.kernel()).active())) {
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
    // ADR-0038: the same queue instance survives storage reload, even at its
    // u64 order boundary; only a new process may establish a new incarnation.
    ayni::kernel::EventIngressTestPeer::exhaust_input_order(
        KernelTestProbe::ingress(work.kernel()));
    const auto body = folder(work);
    const auto accepted = work.create(body);
    ASSERT_EQ(accepted.status, AYNI_OK);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::unknown_deferred);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    settle(work);
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.node().info().boot_epoch, old_epoch + 1U);
    EXPECT_EQ(KernelTestProbe::ingress(work.kernel()).resume_boundary().incarnation, old_epoch);
    EXPECT_EQ(KernelTestProbe::ingress(work.kernel()).resume_boundary().through, UINT64_MAX);
    const ayni_event_v1 event{sizeof(event), AYNI_EVENT_LOCAL_INPUT, text("button")};
    EXPECT_EQ(ayni_post_event(work.kernel(), &event), AYNI_CAPACITY_EXCEEDED);
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

// C-COMMIT-004: a reload counts recovery again from the start of the log,
// but durable_seq never falls below what it was at the fence.
TEST(KernelFence, durable_seq_never_falls_while_a_fence_reloads) {
    WorkNode work;
    work.run();
    ASSERT_EQ(work.create(folder(work)).status, AYNI_OK);
    settle(work);
    const uint64_t durable = work.node().info().durable_seq;
    ASSERT_GT(durable, 0U);
    ASSERT_EQ(work.create(folder(work)).status, AYNI_OK);
    for (int attempt = 0; attempt < 3; ++attempt) {
        work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    }
    bool reloaded = false;
    for (int round = 0; round < 1000; ++round) {
        const auto report = tick(work);
        EXPECT_GE(report.durable_seq, durable) << round;
        EXPECT_GE(work.node().info().durable_seq, durable) << round;
        reloaded = reloaded || work.node().lifecycle() == AYNI_LIFECYCLE_RECOVERING;
        if (reloaded && work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING &&
            KernelTestProbe::log(work.kernel()).idle()) {
            break;
        }
    }
    EXPECT_TRUE(reloaded);
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
}

// The sim Host's fence port, and whether a fence reports one epoch below the
// minimum asked for, as a Host that installed too small an epoch would.
decltype(ayni_host_v1::storage_fence_writers) sim_fence = nullptr;
bool fence_short = false;

ayni_submit_result_t short_fence(void *ctx, uint32_t minimum_epoch, ayni_attempt_ref_v1 attempt,
                                 ayni_completion_t *completion) {
    if (!fence_short) {
        return sim_fence(ctx, minimum_epoch, attempt, completion);
    }
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = attempt;
    result.outcome = AYNI_FENCE_EPOCH_INSTALLED;
    result.value = minimum_epoch - 1U;
    EXPECT_EQ(ayni_completion_post(completion, &result), AYNI_OK);
    return AYNI_SUBMIT_ACCEPTED;
}

// ADR-0027 decision 5: the fence of a RUNNING writer must install an epoch
// above it; less leaves that writer possibly able to store, so the kernel
// restricts and keeps the model for diagnosis.
TEST(KernelFence, isolation_below_the_epoch_asked_for_restricts) {
    WorkNode work;
    sim_fence = work.node().sim().table().storage_fence_writers;
    fence_short = false;
    work.node().replace_fence_port(&short_fence);
    work.run();
    const auto accepted = work.create(folder(work));
    ASSERT_EQ(accepted.status, AYNI_OK);
    for (int attempt = 0; attempt < 3; ++attempt) {
        work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    }
    fence_short = true;
    settle(work);
    fence_short = false;
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(work.node().info().restriction, AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN);
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
// ADR-0038 decision 6: old envelopes are an explicit development boundary.
// Construct CRC-correct unsupported batches; their bodies must never be adopted.
ayni::hosts::sim::StorageImage image_with_schema(uint16_t schema) {
    WorkNode source;
    source.run();
    EXPECT_EQ(source.create(folder(source)).status, AYNI_OK);
    settle(source);
    auto image = source.node().sim().durable_storage();
    for (auto &stored : image.log) {
        ayni::durability::BatchView batch{};
        EXPECT_EQ(ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch),
                  ayni::durability::BatchCheck::ok);
        // Unsupported envelopes are synthetic CRC-valid bytes; there is no old writer.
        constexpr uint32_t schema_at = 4;
        constexpr uint32_t crc_at = ayni::durability::batch_header_size - sizeof(uint32_t);
        ayni::foundation::store_le16(stored.bytes.data() + schema_at, schema);
        ayni::foundation::store_le32(stored.bytes.data() + crc_at,
                                     ayni::foundation::crc32(stored.bytes.data(), crc_at));
    }
    return image;
}

// C-STORE-004/010, ADR-0038: unsupported data is preserved, never reset or migrated.
TEST(StorageSchemaBoundary, unsupported_schema_never_installs_a_writer_or_mutates_source) {
    for (const uint16_t schema :
         {uint16_t{0}, uint16_t{1}, uint16_t{2}, uint16_t{ayni::durability::storage_schema + 1U}}) {
        SCOPED_TRACE(schema);
        const auto image = image_with_schema(schema);
        WorkNode work;
        work.node().sim().durable_storage() = image;
        work.node().create_and_start();
        work.node().recover();
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(work.node().info().restriction, AYNI_RESTRICTION_LOG_UNSUPPORTED);
        const auto &after = work.node().sim().durable_storage();
        ASSERT_EQ(after.log.size(), image.log.size());
        for (std::size_t index = 0; index < image.log.size(); ++index) {
            EXPECT_EQ(after.log[index].bytes, image.log[index].bytes);
        }
        EXPECT_EQ(after.snapshots, image.snapshots);
    }
}

// C-STORE-004/010: restarting the reader cannot silently adopt an older store.
TEST(StorageSchemaBoundary, unsupported_schema_is_still_rejected_after_a_process_restart) {
    WorkNode work;
    work.node().sim().durable_storage() = image_with_schema(1);
    work.node().create_and_start();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    work.node().reboot();
    work.node().recover();
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(work.node().info().restriction, AYNI_RESTRICTION_LOG_UNSUPPORTED);
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
