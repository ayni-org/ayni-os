// Kernel lifecycle through the C ABI (contracts/domain/lifecycles.md,
// ADR-0016): ayni_start, recovery in ayni_step, the durable boot epoch,
// restriction reasons, shutdown and the return-value table. Runs on the
// simulation Host, whose storage completes only when the test says so.

#include "durability/batch/batch_codec.h"
#include "foundation/crc32.h"
#include "foundation/kernel_id.h"
#include "foundation/little_endian.h"
#include "kernel/runtime/boot/boot_sequence.h"
#include "kernel_node.h"
#include "kernel_test_probe.h"
#include "work/agent/agent_records.h"

#include <ayni/ayni.h>
#include <ayni/hosts/sim.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using ayni::kernel::KernelTestProbe;

namespace {

using ayni::durability::BatchIdentity;
using ayni::durability::BatchWriter;
using ayni::durability::RecordHeader;
using ayni::hosts::sim::StorageFault;
using ayni::hosts::sim::StoragePort;
using ayni::hosts::sim::StoredBatch;
using ayni::kernel::boot_epoch_record_type;
using ayni::kernel::boot_epoch_record_version;

using ayni::test::Node;
using ayni::test::smallest_storage_buffer;
using ayni::test::test_storage_buffer_bytes;
using ayni::test::unbounded_items;
using ayni::test::unbounded_time_us;

constexpr uint32_t required = ayni::durability::record_required;

struct Record {
    RecordHeader header;
    std::vector<uint8_t> body;
};

StoredBatch encode(const BatchIdentity &identity, const std::vector<Record> &records) {
    std::size_t body_bytes = 0;
    for (const auto &record : records) {
        body_bytes += record.body.size();
    }
    std::vector<uint8_t> bytes(ayni::durability::batch_size(body_bytes, records.size()));
    BatchWriter writer(bytes.data(), bytes.size(), identity);
    for (const Record &record : records) {
        EXPECT_TRUE(writer.add_record(record.header, record.body.data()));
    }
    bytes.resize(writer.finish());
    return {identity.commit_seq, bytes};
}

Record boot_epoch_record(uint64_t epoch) {
    std::vector<uint8_t> body(8);
    ayni::foundation::store_le64(body.data(), epoch);
    return {{boot_epoch_record_type, boot_epoch_record_version, required, 8}, body};
}

// What a previous boot of this firmware leaves behind.
StoredBatch boot_epoch_batch(uint64_t commit_seq, uint64_t epoch) {
    return encode({1, commit_seq, commit_seq}, {boot_epoch_record(epoch)});
}

// Re-computes the header CRC after a test edits a header field.
void reseal_header(StoredBatch &batch) {
    const std::size_t crc_at = ayni::durability::batch_header_size - 4;
    ayni::foundation::store_le32(batch.bytes.data() + crc_at,
                                 ayni::foundation::crc32(batch.bytes.data(), crc_at));
}

// The single record of a batch the kernel appended, decoded.
struct AppendedEpoch {
    ayni::durability::BatchView batch;
    RecordHeader header;
    uint64_t epoch;
};

AppendedEpoch decode_boot_batch(const StoredBatch &stored) {
    AppendedEpoch out{};
    EXPECT_EQ(ayni::durability::read_batch(stored.bytes.data(), stored.bytes.size(), out.batch),
              ayni::durability::BatchCheck::ok);
    EXPECT_EQ(out.batch.record_count, 1U);
    ayni::durability::RecordReader reader(out.batch);
    const uint8_t *body = nullptr;
    EXPECT_EQ(reader.next(out.header, body), ayni::durability::RecordReader::Next::record);
    if (body != nullptr && out.header.length == 8U) {
        out.epoch = ayni::foundation::load_le64(body);
    }
    return out;
}

// White-box requests that hold completion slots, for the step budget and
// slot exhaustion tests.
struct Recorder {
    static void handle(void *context, const ayni_completion_result_v1 & /*result*/) {
        ++static_cast<Recorder *>(context)->applied;
    }
    int applied = 0;
};

// A port that accepts and leaves the completion to the test.
struct HoldingPort {
    static ayni_submit_result_t call(void *context, ayni_attempt_ref_v1 attempt,
                                     ayni_completion_t *completion) {
        auto &self = *static_cast<HoldingPort *>(context);
        self.held.push_back({attempt, completion});
        return AYNI_SUBMIT_ACCEPTED;
    }
    void post_all() {
        for (const auto &request : held) {
            ayni_completion_result_v1 result{};
            result.struct_size = sizeof(result);
            result.outcome = 1;
            result.attempt = request.attempt;
            EXPECT_EQ(ayni_completion_post(request.completion, &result), AYNI_OK);
        }
        held.clear();
    }
    struct Held {
        ayni_attempt_ref_v1 attempt;
        ayni_completion_t *completion;
    };
    std::vector<Held> held;
};

// Drive only recovery requests; storage deadlines are part of the owner contract.
void recovery_tick(Node &node, bool complete = true) {
    const auto report = node.step(1);
    if (complete) {
        node.sim().complete_storage();
    }
    if (report.more_ready == 0U && node.sim().pending_storage() == 0U &&
        report.next_deadline_us != UINT64_MAX && report.next_deadline_us > node.sim().now_us()) {
        node.sim().advance_us(report.next_deadline_us - node.sim().now_us());
    }
}

void persist_first_fence(Node &node) {
    for (int round = 0; round < 1000 && node.sim().log().empty(); ++round) {
        recovery_tick(node);
    }
    ASSERT_EQ(node.sim().log().size(), 1U);
}

void wait_for_boot_append(Node &node) {
    persist_first_fence(node);
    for (int round = 0; round < 1000 && node.sim().pending_storage() == 0U; ++round) {
        recovery_tick(node, false);
    }
    ASSERT_EQ(node.sim().pending_storage(), 1U);
}

TEST(KernelLifecycle, start_without_storage_is_unsupported_and_stays_configuring) {
    Node node(test_storage_buffer_bytes, false);
    node.create();
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_UNSUPPORTED);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_CONFIGURING);
    EXPECT_EQ(node.destroy(), AYNI_OK);
}

// C-HOST-015: a Host table with only the append/read_prefix pair is valid
// for creation but cannot promise the storage semantics required by the
// current recovery protocol.
TEST(KernelLifecycle, host_with_only_legacy_storage_cannot_start) {
    Node fixture;
    auto host = fixture.sim().table();
    host.storage_read_log = nullptr;
    host.storage_fence_writers = nullptr;
    host.storage_write_snapshot = nullptr;
    host.storage_read_snapshot = nullptr;
    host.storage_trim_prefix = nullptr;
    host.storage_free_bytes = nullptr;
    std::size_t bytes = 0;
    std::size_t alignment = 0;
    ASSERT_EQ(ayni_kernel_memory_requirements(&fixture.config(), &bytes, &alignment), AYNI_OK);
    std::vector<uint64_t> memory(bytes / sizeof(uint64_t) + 1U);
    ayni_kernel_t *kernel = nullptr;
    ASSERT_EQ(ayni_create(&fixture.config(), &host, memory.data(), bytes, &kernel), AYNI_OK);
    EXPECT_EQ(ayni_start(kernel), AYNI_UNSUPPORTED);
    ayni_lifecycle_t state = 0;
    ASSERT_EQ(ayni_get_lifecycle(kernel, &state), AYNI_OK);
    EXPECT_EQ(state, AYNI_LIFECYCLE_CONFIGURING);
    EXPECT_EQ(fixture.sim().pending_storage(), 0U);
    EXPECT_EQ(ayni_destroy(kernel), AYNI_OK);
}

// C-ID-009: the first boot writes epoch 1 in a batch of its own, REQUIRED so
// that an older reader cannot skip it.
TEST(KernelLifecycle, first_boot_persists_epoch_1_before_running) {
    Node node;
    node.create_and_start();
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.info().boot_epoch, 1U);
    EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_NONE);

    ASSERT_EQ(node.sim().log().size(), 2U);
    const AppendedEpoch appended = decode_boot_batch(node.sim().log().back());
    EXPECT_EQ(node.sim().log().back().commit_seq, 2U);
    EXPECT_EQ(appended.batch.identity.commit_seq, 2U);
    EXPECT_EQ(appended.batch.identity.writer_epoch, node.sim().writer_epoch());
    EXPECT_EQ(appended.batch.identity.transition_id, 2U);
    EXPECT_EQ(appended.header.type, boot_epoch_record_type);
    EXPECT_EQ(appended.header.version, boot_epoch_record_version);
    EXPECT_EQ(appended.header.flags, required);
    EXPECT_EQ(appended.epoch, 1U);
    EXPECT_EQ(node.sim().calls_under_lock(), 0U);
}

// C-ID-010: kernel ids embed the epoch in 32 bits, so the last epoch that
// fits is still written. A log at the largest transition id a writer can
// reach still boots, and the boot batch takes the next one.
TEST(KernelLifecycle, largest_epoch_and_transition_id_still_boot) {
    Node node;
    const uint64_t last_transition_id = (uint64_t{1} << 63U) - 1U;
    node.sim().log() = {encode({1, 1, last_transition_id},
                               {boot_epoch_record(ayni::foundation::max_boot_epoch - 1U)})};
    node.create_and_start();
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.info().boot_epoch, ayni::foundation::max_boot_epoch);
    ASSERT_EQ(node.sim().log().size(), 3U);
    EXPECT_EQ(decode_boot_batch(node.sim().log().back()).batch.identity.transition_id,
              last_transition_id + 2U);
}

TEST(KernelLifecycle, each_reboot_persists_the_next_epoch) {
    Node node;
    node.create_and_start();
    node.recover();
    for (uint64_t expected = 2; expected <= 4; ++expected) {
        node.reboot();
        node.recover();
        EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        EXPECT_EQ(node.info().boot_epoch, expected);
    }
    ASSERT_EQ(node.sim().log().size(), 8U);
    const AppendedEpoch last = decode_boot_batch(node.sim().log().back());
    EXPECT_EQ(last.batch.identity.commit_seq, 8U);
    EXPECT_EQ(last.batch.identity.transition_id, 8U);
    EXPECT_EQ(last.epoch, 4U);
}

// RUNNING waits for the append's completion; until then the epoch is not
// reported, and requests carry boot_epoch 0.
TEST(KernelLifecycle, recovery_lasts_until_the_epoch_append_completes) {
    Node node;
    node.create_and_start();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RECOVERING);

    wait_for_boot_append(node);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RECOVERING);
    EXPECT_EQ(node.info().boot_epoch, 0U);
    const uint64_t wakes = node.sim().wake_count();
    node.step();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RECOVERING);
    ASSERT_EQ(node.sim().complete_storage(), 1U);
    EXPECT_GT(node.sim().wake_count(), wakes);
    const auto report = node.step(1);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.info().boot_epoch, 1U);
}

// C-COMMIT-004: durable_seq covers the log read so far during recovery,
// and the boot batch once its append is known durable.
TEST(KernelLifecycle, durable_seq_follows_the_log_read_and_then_the_boot_batch) {
    Node node;
    node.sim().log() = {encode({1, 1, 1}, {boot_epoch_record(1)}),
                        encode({1, 2, 2}, {boot_epoch_record(2)})};
    node.create_and_start();
    EXPECT_EQ(node.info().durable_seq, 0U);
    uint64_t last_seen = 0;
    for (int round = 0; round < 100 && node.lifecycle() == AYNI_LIFECYCLE_RECOVERING; ++round) {
        last_seen = node.info().durable_seq;
        EXPECT_LE(last_seen, 3U);
        recovery_tick(node);
    }
    ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(last_seen, 3U); // fence is durable before the boot append
    EXPECT_EQ(node.info().durable_seq, 4U);
}

TEST(KernelLifecycle, more_ready_while_recovery_has_a_request_due) {
    Node node;
    node.create_and_start();
    node.step();
    node.sim().complete_storage();
    const ayni_step_report_v1 report = node.step(1); // applies the read only
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_EQ(node.sim().pending_storage(), 0U);
}

// Fills the log with BOOT_EPOCH batches, epoch 3 * commit_seq, until it is
// more than twice buffer_bytes, and returns how many.
uint64_t fill_log_beyond(Node &node, uint32_t buffer_bytes) {
    std::size_t log_bytes = 0;
    uint64_t seq = 0;
    while (log_bytes <= std::size_t{buffer_bytes} * 2U) {
        ++seq;
        node.sim().log().push_back(boot_epoch_batch(seq, seq * 3U));
        log_bytes += node.sim().log().back().bytes.size();
    }
    return seq;
}

// The smallest buffer still reads a long log, a few whole batches per read.
TEST(KernelLifecycle, log_longer_than_the_buffer_is_read_in_chunks) {
    Node node;
    node.config().storage_buffer_bytes = smallest_storage_buffer(node.config());
    const uint64_t batches = fill_log_beyond(node, node.config().storage_buffer_bytes);

    node.create_and_start();
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.info().boot_epoch, batches * 3U + 1U);
    const AppendedEpoch appended = decode_boot_batch(node.sim().log().back());
    EXPECT_EQ(appended.batch.identity.commit_seq, batches + 2U);
    EXPECT_EQ(appended.batch.identity.transition_id, batches + 2U);
}

// Optional records the kernel does not know are skipped (C-STORE-004); the
// appended batch keeps the largest writer_epoch and transition_id seen.
TEST(KernelLifecycle, unknown_optional_records_are_skipped) {
    Node node;
    node.sim().log().push_back(encode({1, 1, 7}, {{{999, 1, 0, 2}, {1, 2}}, boot_epoch_record(5)}));
    node.sim().log().push_back(encode({2, 2, 3}, {{{boot_epoch_record_type, 9, 0, 3}, {0, 0, 0}}}));
    node.create_and_start();
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.info().boot_epoch, 6U);
    const AppendedEpoch appended = decode_boot_batch(node.sim().log().back());
    EXPECT_EQ(appended.batch.identity.commit_seq, 4U);
    EXPECT_EQ(appended.batch.identity.writer_epoch, 3U);
    EXPECT_EQ(appended.batch.identity.transition_id, 9U);
}

// A later version of a type this reader knows is unknown to it too:
// without REQUIRED it is skipped, not read as the version it knows.
TEST(KernelLifecycle, an_optional_record_of_a_newer_version_is_skipped) {
    Node node;
    node.sim().log().push_back(encode(
        {1, 1, 7}, {{{ayni::work::instance_record_type, 2, 0, 2}, {1, 2}}, boot_epoch_record(5)}));
    node.create_and_start();
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.info().boot_epoch, 6U);
}

struct LogCase {
    const char *name;
    std::vector<StoredBatch> log;
    ayni_restriction_t restriction;
};

std::vector<LogCase> bad_logs() {
    std::vector<LogCase> cases;
    {
        StoredBatch damaged = boot_epoch_batch(2, 2);
        damaged.bytes[50] ^= 0x01U;
        cases.push_back(
            {"damaged batch", {boot_epoch_batch(1, 1), damaged}, AYNI_RESTRICTION_LOG_CORRUPT});
    }
    cases.push_back({"commit_seq gap",
                     {boot_epoch_batch(1, 1), boot_epoch_batch(3, 2)},
                     AYNI_RESTRICTION_LOG_CORRUPT});
    cases.push_back(
        {"log not starting at 1", {boot_epoch_batch(2, 1)}, AYNI_RESTRICTION_LOG_CORRUPT});
    {
        // The Host frames it as 2, but the batch itself says 3.
        StoredBatch misframed = boot_epoch_batch(3, 2);
        misframed.commit_seq = 2;
        cases.push_back({"Host and batch disagree on commit_seq",
                         {boot_epoch_batch(1, 1), misframed},
                         AYNI_RESTRICTION_LOG_CORRUPT});
    }
    cases.push_back({"epoch not growing",
                     {boot_epoch_batch(1, 4), boot_epoch_batch(2, 4)},
                     AYNI_RESTRICTION_LOG_CORRUPT});
    cases.push_back({"epoch 0", {boot_epoch_batch(1, 0)}, AYNI_RESTRICTION_LOG_CORRUPT});
    cases.push_back({"older writer after a newer one",
                     {encode({2, 1, 1}, {boot_epoch_record(1)}), encode({1, 2, 2}, {})},
                     AYNI_RESTRICTION_LOG_CORRUPT});
    cases.push_back({"last transition_id used up",
                     {encode({1, 1, std::numeric_limits<uint64_t>::max()}, {boot_epoch_record(1)})},
                     AYNI_RESTRICTION_LOG_CORRUPT});
    cases.push_back({"transition_id no writer reaches",
                     {encode({1, 1, uint64_t{1} << 63U}, {boot_epoch_record(1)})},
                     AYNI_RESTRICTION_LOG_CORRUPT});
    cases.push_back(
        {"BOOT_EPOCH body of the wrong length",
         {encode({1, 1, 1}, {{{boot_epoch_record_type, boot_epoch_record_version, required, 4},
                              {1, 0, 0, 0}}})},
         AYNI_RESTRICTION_LOG_CORRUPT});
    cases.push_back({"unknown required record",
                     {encode({1, 1, 1}, {{{999, 1, required, 0}, {}}})},
                     AYNI_RESTRICTION_LOG_UNSUPPORTED});
    cases.push_back(
        {"unknown required version",
         {encode({1, 1, 1}, {{{boot_epoch_record_type, 2, required, 8}, std::vector<uint8_t>(8)}})},
         AYNI_RESTRICTION_LOG_UNSUPPORTED});
    cases.push_back({"unknown record flag bit",
                     {encode({1, 1, 1}, {{{999, 1, 0x2U, 0}, {}}})},
                     AYNI_RESTRICTION_LOG_UNSUPPORTED});
    {
        StoredBatch newer = boot_epoch_batch(1, 1);
        ayni::foundation::store_le16(newer.bytes.data() + 4,
                                     ayni::durability::storage_schema + 1U); // unsupported schema
        reseal_header(newer);
        cases.push_back({"other storage_schema", {newer}, AYNI_RESTRICTION_LOG_UNSUPPORTED});
    }
    cases.push_back({"last epoch exhausted",
                     {boot_epoch_batch(1, std::numeric_limits<uint64_t>::max())},
                     AYNI_RESTRICTION_BOOT_EPOCH_EXHAUSTED});
    cases.push_back({"last epoch that kernel ids can embed",
                     {boot_epoch_batch(1, ayni::foundation::max_boot_epoch)},
                     AYNI_RESTRICTION_BOOT_EPOCH_EXHAUSTED});
    return cases;
}

// C-STORE-003, C-STORE-004, C-STORE-009: a log the kernel cannot trust or
// does not understand stops recovery before any epoch is written.
TEST(KernelLifecycle, untrusted_or_unknown_log_restricts_without_writing) {
    for (const LogCase &bad : bad_logs()) {
        SCOPED_TRACE(bad.name);
        Node node;
        node.sim().log() = bad.log;
        node.create_and_start();
        node.recover();
        EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(node.info().restriction, bad.restriction);
        EXPECT_EQ(node.info().boot_epoch, 0U);
        EXPECT_EQ(node.sim().log().size(), bad.log.size());
    }
}

struct FaultCase {
    const char *name;
    StoragePort port;
    StorageFault fault;
    ayni_restriction_t restriction;
    std::size_t log_size_after;
};

TEST(KernelLifecycle, storage_faults_restrict_with_their_reason) {
    const FaultCase cases[] = {
        {"read rejected", StoragePort::read_prefix, StorageFault::reject,
         AYNI_RESTRICTION_STORAGE_REJECTED, 0},
        {"read failed", StoragePort::read_prefix, StorageFault::fail,
         AYNI_RESTRICTION_STORAGE_READ_FAILED, 0},
        {"read unknown", StoragePort::read_prefix, StorageFault::unknown_lost,
         AYNI_RESTRICTION_STORAGE_READ_FAILED, 0},
        {"append rejected", StoragePort::append, StorageFault::reject,
         AYNI_RESTRICTION_STORAGE_REJECTED, 0},
        {"append failed", StoragePort::append, StorageFault::fail,
         AYNI_RESTRICTION_STORAGE_WRITE_FAILED, 0},
        {"append unknown, lost", StoragePort::append, StorageFault::unknown_lost,
         AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN, 0},
        {"append unknown, stored", StoragePort::append, StorageFault::unknown_persisted,
         AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN, 1},
    };
    for (const FaultCase &fault : cases) {
        SCOPED_TRACE(fault.name);
        Node node;
        for (int attempt = 0; attempt < (fault.port == StoragePort::append ? 3 : 1); ++attempt) {
            node.sim().inject_fault(fault.port,
                                    attempt > 0 && fault.fault == StorageFault::unknown_persisted
                                        ? StorageFault::unknown_lost
                                        : fault.fault);
        }
        node.create_and_start();
        node.recover();
        EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        EXPECT_EQ(node.info().restriction, fault.restriction);
        EXPECT_EQ(node.info().boot_epoch, 0U);
        // An append whose result is unknown is not reported durable, even
        // when the store kept it.
        EXPECT_EQ(node.info().durable_seq, 0U);
        EXPECT_EQ(node.sim().log().size(), fault.log_size_after);
        EXPECT_EQ(KernelTestProbe::completions(node.kernel()).in_flight(), 0U);
    }
}

// The buffer bounds the largest batch: the Host cannot deliver a bigger one.
TEST(KernelLifecycle, batch_larger_than_the_buffer_reports_capacity) {
    Node node;
    const uint32_t buffer = smallest_storage_buffer(node.config());
    node.config().storage_buffer_bytes = buffer;
    node.sim().log().push_back(
        encode({1, 1, 1}, {{{999, 1, 0, buffer}, std::vector<uint8_t>(buffer)}}));
    node.create_and_start();
    node.recover();
    EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY);
}

// Holds one read and lets the test answer it.
struct HeldRead {
    static ayni_submit_result_t call(void * /*ctx*/, uint64_t /*from_commit_seq*/, uint8_t *buffer,
                                     uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                     ayni_completion_t *completion) {
        last = {attempt, completion, buffer_size, buffer};
        return AYNI_SUBMIT_ACCEPTED;
    }
    ayni_attempt_ref_v1 attempt;
    ayni_completion_t *completion;
    uint32_t buffer_size;
    uint8_t *buffer;
    static HeldRead last;
};
HeldRead HeldRead::last{};

// A completion claiming more bytes than the buffer holds is not trusted.
TEST(KernelLifecycle, read_count_beyond_the_buffer_restricts_as_read_failed) {
    Node node;
    HeldRead::last = {};
    node.replace_read_port(&HeldRead::call);
    node.create_and_start();
    for (int round = 0; round < 10 && HeldRead::last.buffer_size == 0U; ++round) {
        recovery_tick(node);
    }
    ASSERT_EQ(HeldRead::last.buffer_size, test_storage_buffer_bytes);
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.outcome = AYNI_WRITE_OK;
    result.attempt = HeldRead::last.attempt;
    result.value = HeldRead::last.buffer_size + 1U;
    ASSERT_EQ(ayni_completion_post(HeldRead::last.completion, &result), AYNI_OK);
    node.step();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_STORAGE_READ_FAILED);
}

// A physical cursor must advance when the Host returns a nonempty chunk;
// accepting it otherwise would let recovery spin forever on the same bytes.
TEST(KernelLifecycle, nonadvancing_physical_read_is_rejected) {
    Node node;
    HeldRead::last = {};
    node.replace_read_port(&HeldRead::call);
    node.create_and_start();
    for (int round = 0; round < 10 && HeldRead::last.buffer_size == 0U; ++round) {
        recovery_tick(node);
    }
    ASSERT_NE(HeldRead::last.buffer, nullptr);
    const StoredBatch stored = boot_epoch_batch(1, 1);
    std::memcpy(HeldRead::last.buffer, stored.bytes.data(), stored.bytes.size());
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.outcome = AYNI_WRITE_OK;
    result.attempt = HeldRead::last.attempt;
    result.value = stored.bytes.size();
    result.next_cursor = 0;
    ASSERT_EQ(ayni_completion_post(HeldRead::last.completion, &result), AYNI_OK);
    node.step();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_STORAGE_READ_FAILED);
}

// Recovery requests carry boot_epoch 0, since no epoch of this boot is
// durable yet, and distinct request ids; the first request after RUNNING
// carries the new epoch.
TEST(KernelLifecycle, recovery_requests_use_boot_epoch_0) {
    Node node;
    node.config().storage_buffer_bytes = smallest_storage_buffer(node.config());
    const uint64_t batches = fill_log_beyond(node, node.config().storage_buffer_bytes);
    node.create_and_start();
    node.recover();
    ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    const std::vector<ayni_attempt_ref_v1> &attempts = node.sim().storage_attempts();
    ASSERT_GE(attempts.size(), 4U); // at least two chunks, the end of log and the append
    for (std::size_t index = 0; index < attempts.size(); ++index) {
        EXPECT_EQ(attempts[index].boot_epoch, 0U) << index;
        EXPECT_EQ(attempts[index].request_id, index + 1U) << index;
    }

    Recorder recorder;
    HoldingPort port;
    ASSERT_EQ(
        KernelTestProbe::completions(node.kernel())
            .issue(node.info().boot_epoch, &Recorder::handle, &recorder, &HoldingPort::call, &port),
        ayni::kernel::IssueResult::in_flight);
    ASSERT_EQ(port.held.size(), 1U);
    EXPECT_EQ(port.held[0].attempt.boot_epoch, batches * 3U + 1U);
    EXPECT_EQ(port.held[0].attempt.request_id, attempts.size() + 1U);
    port.post_all();
    node.step();
    EXPECT_EQ(recorder.applied, 1);
}

// An append that ends UNKNOWN may be stored. If it was, the next boot reads
// it and moves past that epoch, so no two boots share one.
TEST(KernelLifecycle, reboot_after_an_unknown_append_skips_its_epoch) {
    Node node;
    node.create_and_start();
    persist_first_fence(node);
    for (int attempt = 0; attempt < 3; ++attempt) {
        node.sim().inject_fault(StoragePort::append, attempt == 0 ? StorageFault::unknown_persisted
                                                                  : StorageFault::unknown_lost);
    }
    node.recover();
    ASSERT_EQ(node.info().restriction, AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN);
    node.reboot();
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.info().boot_epoch, 2U);
    ASSERT_EQ(node.sim().log().size(), 4U);
    EXPECT_EQ(decode_boot_batch(node.sim().log().back()).batch.identity.commit_seq, 4U);
}

// With every slot taken, recovery waits without asking to be called again;
// the post that frees a slot wakes the owner, and recovery goes on.
TEST(KernelLifecycle, recovery_waits_for_a_free_slot_without_spinning) {
    Node node;
    node.create_and_start();
    Recorder recorder;
    HoldingPort port;
    for (int request = 0; request < 4; ++request) { // completion_slot_count
        ASSERT_EQ(KernelTestProbe::completions(node.kernel())
                      .issue(0, &Recorder::handle, &recorder, &HoldingPort::call, &port),
                  ayni::kernel::IssueResult::in_flight);
    }
    ayni_step_report_v1 report = node.step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(node.sim().pending_storage(), 0U);

    const uint64_t wakes = node.sim().wake_count();
    port.post_all();
    EXPECT_GT(node.sim().wake_count(), wakes);
    report = node.step();
    EXPECT_EQ(recorder.applied, 4);
    EXPECT_EQ(node.sim().pending_storage(), 1U); // the first read
    node.recover();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
}

TEST(KernelLifecycle, restriction_is_reported_only_while_restricted) {
    Node node;
    node.sim().inject_fault(StoragePort::read_prefix, StorageFault::fail);
    node.create_and_start();
    node.recover();
    ASSERT_EQ(node.info().restriction, AYNI_RESTRICTION_STORAGE_READ_FAILED);
    EXPECT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
    EXPECT_EQ(node.info().lifecycle, AYNI_LIFECYCLE_DRAINING);
    EXPECT_EQ(node.info().restriction, AYNI_RESTRICTION_NONE);
}

// C-HOST-018, C-LIFE-002: STOPPED, and with it ayni_destroy, waits for every
// accepted request to be posted; no new request starts once DRAINING.
TEST(KernelLifecycle, shutdown_during_recovery_waits_for_the_request_in_flight) {
    Node node;
    node.create_and_start();
    node.step(); // read in flight
    ASSERT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_DRAINING);

    ayni_step_report_v1 report = node.step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_DRAINING);
    EXPECT_EQ(node.destroy(), AYNI_INVALID_STATE);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_DRAINING);

    ASSERT_EQ(node.sim().complete_storage(), 1U);
    report = node.step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_EQ(node.sim().pending_storage(), 0U); // the append was never issued
    EXPECT_TRUE(node.sim().log().empty());
    EXPECT_EQ(node.info().boot_epoch, 0U);
    EXPECT_EQ(node.destroy(), AYNI_OK);
}

// An append already accepted still completes during DRAINING; if it succeeds
// the epoch is durable and reported, though RUNNING is never entered.
TEST(KernelLifecycle, append_completing_while_draining_is_still_applied) {
    Node node;
    node.create_and_start();
    wait_for_boot_append(node);
    ASSERT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
    ASSERT_EQ(node.sim().complete_storage(), 1U);
    node.step();
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_EQ(node.info().boot_epoch, 1U);
    EXPECT_EQ(node.sim().log().size(), 2U);
}

TEST(KernelLifecycle, shutdown_from_running_and_restricted_stops_in_one_step) {
    for (const bool restricted : {false, true}) {
        SCOPED_TRACE(restricted ? "RESTRICTED" : "RUNNING");
        Node node;
        if (restricted) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                node.sim().inject_fault(StoragePort::append, StorageFault::fail);
            }
        }
        node.create_and_start();
        node.recover();
        ASSERT_EQ(node.lifecycle(),
                  restricted ? AYNI_LIFECYCLE_RESTRICTED : AYNI_LIFECYCLE_RUNNING);
        ASSERT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
        EXPECT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
        node.step();
        EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_STOPPED);
        EXPECT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
        EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_STOPPED);
    }
}

// --- Return values per state (contracts/domain/lifecycles.md) -----------

enum class Call { get_lifecycle, get_kernel_info, destroy, start, step, begin_shutdown };

ayni_status_t invoke(Node &node, Call call) {
    ayni_kernel_t *kernel = node.kernel();
    switch (call) {
    case Call::get_lifecycle: {
        ayni_lifecycle_t state = 0;
        return ayni_get_lifecycle(kernel, &state);
    }
    case Call::get_kernel_info: {
        ayni_kernel_info_v1 info{};
        info.struct_size = sizeof(info);
        return ayni_get_kernel_info(kernel, &info);
    }
    case Call::destroy:
        return node.destroy();
    case Call::start:
        return ayni_start(kernel);
    case Call::step:
        return ayni_step(kernel, unbounded_items, unbounded_time_us, nullptr);
    case Call::begin_shutdown:
        return ayni_begin_shutdown(kernel);
    }
    return AYNI_INTERNAL;
}

TEST(KernelLifecycle, return_values_follow_the_state_table) {
    constexpr ayni_status_t ok = AYNI_OK;
    constexpr ayni_status_t bad = AYNI_INVALID_STATE;
    const ayni_lifecycle_t states[] = {
        AYNI_LIFECYCLE_CONFIGURING, AYNI_LIFECYCLE_RECOVERING, AYNI_LIFECYCLE_RUNNING,
        AYNI_LIFECYCLE_RESTRICTED,  AYNI_LIFECYCLE_DRAINING,   AYNI_LIFECYCLE_STOPPED,
    };
    const struct {
        Call call;
        const char *name;
        std::array<ayni_status_t, 6> expected;
    } table[] = {
        {Call::get_lifecycle, "ayni_get_lifecycle", {ok, ok, ok, ok, ok, ok}},
        {Call::get_kernel_info, "ayni_get_kernel_info", {ok, ok, ok, ok, ok, ok}},
        {Call::destroy, "ayni_destroy", {ok, bad, bad, bad, bad, ok}},
        {Call::start, "ayni_start", {ok, bad, bad, bad, bad, bad}},
        {Call::step, "ayni_step", {bad, ok, ok, ok, ok, bad}},
        {Call::begin_shutdown, "ayni_begin_shutdown", {bad, ok, ok, ok, ok, ok}},
    };
    for (const auto &row : table) {
        for (std::size_t column = 0; column < 6; ++column) {
            SCOPED_TRACE(::testing::Message() << row.name << " in state " << states[column]);
            Node node;
            node.enter(states[column]);
            const ayni_lifecycle_t before = node.lifecycle();
            const ayni_status_t status = invoke(node, row.call);
            EXPECT_EQ(status, row.expected[column]);
            if (status == AYNI_INVALID_STATE) {
                EXPECT_EQ(node.lifecycle(), before); // an illegal call changes nothing
            }
        }
    }
}

// --- Argument checks -----------------------------------------------------

TEST(KernelLifecycle, null_kernel_is_invalid_argument) {
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_kernel_info(nullptr, &info), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_start(nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_step(nullptr, 1, 1, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_begin_shutdown(nullptr), AYNI_INVALID_ARGUMENT);
}

TEST(KernelLifecycle, kernel_info_checks_its_output) {
    Node node;
    node.create();
    EXPECT_EQ(ayni_get_kernel_info(node.kernel(), nullptr), AYNI_INVALID_ARGUMENT);
    // ADR-0053: an older layout, such as that of 0.4 ending after boot_epoch,
    // is refused like any shorter size.
    ayni_kernel_info_v1 info{};
    for (const uint32_t size : {static_cast<uint32_t>(offsetof(ayni_kernel_info_v1, durable_seq)),
                                static_cast<uint32_t>(sizeof(info) - 1U)}) {
        std::memset(&info, 0xA5, sizeof(info));
        info.struct_size = size;
        EXPECT_EQ(ayni_get_kernel_info(node.kernel(), &info), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(info.boot_epoch, 0xA5A5A5A5A5A5A5A5U);
    }
    info.struct_size = sizeof(info) + 8;
    EXPECT_EQ(ayni_get_kernel_info(node.kernel(), &info), AYNI_UNSUPPORTED_VERSION);

    std::memset(&info, 0xA5, sizeof(info));
    info.struct_size = sizeof(info);
    ASSERT_EQ(ayni_get_kernel_info(node.kernel(), &info), AYNI_OK);
    EXPECT_EQ(info.struct_size, sizeof(info));
    EXPECT_EQ(info.lifecycle, AYNI_LIFECYCLE_CONFIGURING);
    EXPECT_EQ(info.restriction, AYNI_RESTRICTION_NONE);
    EXPECT_EQ(info.reserved, 0U);
    EXPECT_EQ(info.boot_epoch, 0U);
    EXPECT_EQ(info.durable_seq, 0U);
    EXPECT_EQ(info.events_unmatched, 0U);
    EXPECT_EQ(info.events_discarded, 0U);
}

// A rejected step has no effect: no storage request is issued.
TEST(KernelLifecycle, step_rejects_bad_arguments_without_effect) {
    Node node;
    node.create_and_start();
    ayni_step_report_v1 report{};
    report.struct_size = sizeof(report);
    EXPECT_EQ(ayni_step(node.kernel(), 0, 1, &report), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_step(node.kernel(), 1, 0, &report), AYNI_INVALID_ARGUMENT);
    // Neither an older layout, such as that of 0.2 before durable_seq, nor any
    // other shorter size (ADR-0053).
    report.struct_size = offsetof(ayni_step_report_v1, durable_seq);
    EXPECT_EQ(ayni_step(node.kernel(), 1, 1, &report), AYNI_INVALID_ARGUMENT);
    report.struct_size = sizeof(report) - 1U;
    EXPECT_EQ(ayni_step(node.kernel(), 1, 1, &report), AYNI_INVALID_ARGUMENT);
    report.struct_size = sizeof(report) + 8;
    EXPECT_EQ(ayni_step(node.kernel(), 1, 1, &report), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(node.sim().pending_storage(), 0U);

    EXPECT_EQ(ayni_step(node.kernel(), 1, 1, nullptr), AYNI_OK); // report is optional
    EXPECT_EQ(node.sim().pending_storage(), 1U);
}

TEST(KernelLifecycle, step_reports_durable_seq) {
    Node node;
    node.create_and_start();
    node.recover();
    ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    ayni_step_report_v1 report{};
    std::memset(&report, 0xA5, sizeof(report));
    report.struct_size = sizeof(report);
    ASSERT_EQ(ayni_step(node.kernel(), 1, 1, &report), AYNI_OK);
    EXPECT_EQ(report.struct_size, sizeof(report));
    EXPECT_EQ(report.durable_seq, 2U); // the boot batch
    EXPECT_EQ(report.durable_seq, node.info().durable_seq);
}

// --- Step budget (white-box: several completions posted at once) ---------

void post_four(Node &node, Recorder &recorder, HoldingPort &port) {
    for (int request = 0; request < 4; ++request) {
        ASSERT_EQ(KernelTestProbe::completions(node.kernel())
                      .issue(node.info().boot_epoch, &Recorder::handle, &recorder,
                             &HoldingPort::call, &port),
                  ayni::kernel::IssueResult::in_flight);
    }
    port.post_all();
}

TEST(KernelLifecycle, step_applies_at_most_max_items) {
    Node node;
    node.create_and_start();
    node.recover();
    Recorder recorder;
    HoldingPort port;
    post_four(node, recorder, port);

    ayni_step_report_v1 report = node.step(3);
    EXPECT_EQ(report.items_done, 3U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_EQ(recorder.applied, 3);
    report = node.step(3);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(recorder.applied, 4);
}

// The clock is read after each item; once max_time_us has passed the step
// returns, however many items remain.
TEST(KernelLifecycle, step_returns_once_its_time_budget_is_spent) {
    Node node;
    node.create_and_start();
    node.recover();
    Recorder recorder;
    HoldingPort port;
    post_four(node, recorder, port);

    node.sim().advance_per_read_us(10);
    ayni_step_report_v1 report = node.step(unbounded_items, 20);
    EXPECT_EQ(report.items_done, 2U); // reads at 0 (start), 10, 20
    EXPECT_EQ(report.more_ready, 1U);
    report = node.step(unbounded_items, 25);
    EXPECT_EQ(report.items_done, 2U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(recorder.applied, 4);
}

} // namespace
