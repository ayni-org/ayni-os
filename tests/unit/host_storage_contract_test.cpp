// Shared Host storage contract suite (C-HOST-017): every scenario runs
// unchanged on each Host that provides the log ports, the sim Host and the
// Native file Host, through ayni_host_v1 (storage_hosts.h). Where a Host may
// legitimately differ, the scenario asks it (checks_frames) instead of
// naming it. The last scenarios run recovery on a real kernel.

#include "durability/batch/batch_codec.h"
#include "storage_hosts.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace {

using ayni::kernel::IssueResult;
using ayni::test::Bytes;
using ayni::test::joined;
using ayni::test::StorageHost;
using ayni::test::StorageRequest;
using ayni::test::StorageRequests;
using ayni::test::test_batch;

enum class HostKind { sim, native_file };

const char *name_of(HostKind kind) {
    return kind == HostKind::sim ? "sim" : "native_file";
}

void PrintTo(HostKind kind, std::ostream *out) {
    *out << name_of(kind);
}

std::string host_name(const ::testing::TestParamInfo<HostKind> &info) {
    return name_of(info.param);
}

class HostStorage : public ::testing::TestWithParam<HostKind> {
  protected:
    // Some scenarios need a fresh log per round.
    std::unique_ptr<StorageHost> make_host() const {
        if (GetParam() == HostKind::sim) {
            return std::make_unique<ayni::test::SimStorage>();
        }
        return std::make_unique<ayni::test::NativeFileStorage>();
    }
};

INSTANTIATE_TEST_SUITE_P(Hosts, HostStorage,
                         ::testing::Values(HostKind::sim, HostKind::native_file), host_name);

bool same_attempt(const ayni_attempt_ref_v1 &a, const ayni_attempt_ref_v1 &b) {
    return a.boot_epoch == b.boot_epoch && a.request_id == b.request_id;
}

// Whether bytes is batches[0..n) joined for some n: whole batches, in log
// order, with none skipped.
bool whole_batch_prefix(const Bytes &bytes, const std::vector<Bytes> &batches) {
    Bytes prefix;
    for (std::size_t count = 0;; ++count) {
        if (prefix == bytes) {
            return true;
        }
        if (count == batches.size() || prefix.size() >= bytes.size()) {
            return false;
        }
        prefix.insert(prefix.end(), batches[count].begin(), batches[count].end());
    }
}

TEST_P(HostStorage, append_then_read_returns_the_same_bytes) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    EXPECT_EQ(requests.read_now(1), Bytes{}); // an empty log

    const Bytes batch = test_batch(1, 40);
    EXPECT_TRUE(requests.append_now(1, batch));
    EXPECT_EQ(requests.read_now(1), batch);
    EXPECT_EQ(requests.read_now(2), Bytes{}); // no batch is left
}

TEST_P(HostStorage, read_from_a_later_commit_seq_skips_earlier_batches) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const std::vector<Bytes> batches{test_batch(1, 30), test_batch(2, 50), test_batch(3, 20)};
    for (uint64_t seq = 1; seq <= 3; ++seq) {
        ASSERT_TRUE(requests.append_now(seq, batches[seq - 1U]));
    }
    EXPECT_EQ(requests.read_now(1), joined({batches[0], batches[1], batches[2]}));
    EXPECT_EQ(requests.read_now(2), joined({batches[1], batches[2]}));
    EXPECT_EQ(requests.read_now(3), batches[2]);
    EXPECT_EQ(requests.read_now(4), Bytes{});
}

// The Host keeps the order of the appends and compares commit_seq only to
// skip; making sense of a log whose commit_seq goes back is the kernel's
// job (C-STORE-003).
TEST_P(HostStorage, reads_follow_log_order_whatever_the_commit_seq) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const Bytes fifth = test_batch(5, 30);
    const Bytes third = test_batch(3, 40);
    const Bytes seventh = test_batch(7, 50);
    ASSERT_TRUE(requests.append_now(5, fifth));
    ASSERT_TRUE(requests.append_now(3, third));
    ASSERT_TRUE(requests.append_now(7, seventh));
    EXPECT_EQ(requests.read_now(1), joined({fifth, third, seventh}));
    EXPECT_EQ(requests.read_now(4), joined({fifth, seventh}));
    EXPECT_EQ(requests.read_now(1, 30), fifth);
    EXPECT_EQ(requests.read_now(6), seventh);
    EXPECT_EQ(requests.read_now(8), Bytes{});
    EXPECT_EQ(requests.read_now(3, 40), fifth); // the first one at or after 3 in log order
}

TEST_P(HostStorage, only_whole_batches_fit_the_buffer) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const std::vector<Bytes> batches{test_batch(1, 100), test_batch(2, 100), test_batch(3, 100)};
    for (uint64_t seq = 1; seq <= 3; ++seq) {
        ASSERT_TRUE(requests.append_now(seq, batches[seq - 1U]));
    }
    EXPECT_EQ(requests.read_now(1, 250), joined({batches[0], batches[1]}));
    EXPECT_EQ(requests.read_now(3, 250), batches[2]);
    EXPECT_EQ(requests.read_now(1, 300), joined({batches[0], batches[1], batches[2]}));
    EXPECT_EQ(requests.read_now(4, 250), Bytes{});
}

TEST_P(HostStorage, first_batch_larger_than_the_buffer_reports_its_required_size) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const Bytes small = test_batch(1, 50);
    const Bytes large = test_batch(2, 300);
    ASSERT_TRUE(requests.append_now(1, small));
    ASSERT_TRUE(requests.append_now(2, large));

    EXPECT_EQ(requests.read_now(1, 256), small); // the large one waits for the next read
    StorageRequest &too_small = requests.read(2, 256);
    requests.finish();
    EXPECT_EQ(too_small.applied, 1);
    EXPECT_EQ(too_small.result.outcome, AYNI_READ_BUFFER_TOO_SMALL);
    EXPECT_EQ(too_small.result.value, large.size());
    EXPECT_EQ(too_small.result.next_cursor, 0U);
    EXPECT_EQ(requests.read_now(2, 400), large);
}

Bytes identified_batch(uint32_t epoch, uint64_t transition, uint64_t seq, uint8_t value = 1) {
    Bytes bytes(128);
    ayni::durability::BatchWriter writer(bytes.data(), bytes.size(), {epoch, seq, transition});
    EXPECT_TRUE(writer.add_record({900, 1, 0, 1}, &value));
    bytes.resize(writer.finish());
    return bytes;
}

// C-COMMIT-008: the same identity acknowledges only the same full bytes,
// whether the first completion was lost or its writer has reopened.
TEST_P(HostStorage, repeated_batch_identity_keeps_one_copy_and_refuses_changed_bytes) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const Bytes batch = identified_batch(1, 20, 1);
    StorageRequest &first = requests.append_identity(1, 20, 1, batch);
    requests.finish();
    ASSERT_EQ(first.result.outcome, AYNI_WRITE_OK);
    StorageRequest &retry = requests.append_identity(1, 20, 1, batch);
    requests.finish();
    EXPECT_EQ(retry.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(requests.read_now(1), batch);
    StorageRequest &changed = requests.append_identity(1, 20, 1, identified_batch(1, 20, 1, 2));
    requests.finish();
    EXPECT_EQ(changed.result.outcome, AYNI_WRITE_FAILED);
    host->stop();
    host->start();
    requests.reset();
    StorageRequest &reopened_retry = requests.append_identity(1, 20, 1, batch);
    requests.finish();
    EXPECT_EQ(reopened_retry.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(requests.read_now(1), batch);
}

// C-STORE-005: cursors preserve physical order even when stale writers put
// a lower sequence after a higher one. The Kernel owns their interpretation.
TEST_P(HostStorage, physical_cursor_reads_every_frame_without_filtering_its_sequence) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const Bytes fifth = test_batch(5, 40);
    const Bytes third = test_batch(3, 30);
    const Bytes seventh = test_batch(7, 50);
    ASSERT_TRUE(requests.append_now(5, fifth));
    ASSERT_TRUE(requests.append_now(3, third));
    ASSERT_TRUE(requests.append_now(7, seventh));
    StorageRequest &small = requests.physical_read(0, 39);
    requests.finish();
    EXPECT_EQ(small.result.outcome, AYNI_READ_BUFFER_TOO_SMALL);
    EXPECT_EQ(small.result.value, fifth.size());
    StorageRequest &first = requests.physical_read(0, 40);
    requests.finish();
    ASSERT_EQ(first.result.outcome, AYNI_WRITE_OK);
    ASSERT_NE(first.result.next_cursor, 0U);
    EXPECT_EQ(first.received, fifth);
    StorageRequest &next = requests.physical_read(first.result.next_cursor, 80);
    requests.finish();
    EXPECT_EQ(next.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(next.received, joined({third, seventh}));
    StorageRequest &end = requests.physical_read(next.result.next_cursor, 80);
    requests.finish();
    EXPECT_EQ(end.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(end.result.value, 0U);
}

// C-HOST-012/C-STORE-005: the isolation boundary applies to every mutating
// port, and repeated fence calls allocate strictly newer epochs.
TEST_P(HostStorage, writer_fence_isolates_append_snapshot_and_trim) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    StorageRequest &fence = requests.fence(10);
    requests.finish();
    ASSERT_TRUE(fence.result.outcome == AYNI_FENCE_DRAINED ||
                fence.result.outcome == AYNI_FENCE_EPOCH_INSTALLED);
    ASSERT_GE(fence.result.value, 10U);
    const uint32_t epoch = static_cast<uint32_t>(fence.result.value);
    StorageRequest &old_append = requests.append_identity(1, 1, 1, identified_batch(1, 1, 1));
    StorageRequest &old_snapshot = requests.write_snapshot(1, 0, 0, {1, 2});
    StorageRequest &old_trim = requests.trim(1, 1, 1);
    requests.finish();
    EXPECT_EQ(old_append.result.outcome, AYNI_WRITE_FAILED);
    EXPECT_EQ(old_snapshot.result.outcome, AYNI_WRITE_FAILED);
    EXPECT_EQ(old_trim.result.outcome, AYNI_WRITE_FAILED);
    StorageRequest &new_append =
        requests.append_identity(epoch, 1, 1, identified_batch(epoch, 1, 1));
    requests.finish();
    EXPECT_EQ(new_append.result.outcome, AYNI_WRITE_OK);
    StorageRequest &second_fence = requests.fence(2);
    requests.finish();
    EXPECT_GT(second_fence.result.value, epoch);
}

// C-STORE-006: slots are independent, chunk writes and read-back survive
// a new Host, and an identical chunk retry cannot truncate later chunks.
TEST_P(HostStorage, snapshot_slots_keep_sequential_chunks_and_idempotent_retries) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    StorageRequest &absent = requests.read_snapshot(0, 0, 8);
    requests.finish();
    ASSERT_EQ(absent.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(absent.result.value, 0U);
    StorageRequest &first = requests.write_snapshot(1, 0, 0, {1, 2, 3});
    requests.finish();
    ASSERT_EQ(first.result.outcome, AYNI_WRITE_OK);
    StorageRequest &second = requests.write_snapshot(1, 0, 3, {4, 5});
    requests.finish();
    ASSERT_EQ(second.result.outcome, AYNI_WRITE_OK);
    StorageRequest &retry = requests.write_snapshot(1, 0, 0, {1, 2, 3});
    StorageRequest &other = requests.write_snapshot(1, 1, 0, {8, 9});
    requests.finish();
    EXPECT_EQ(retry.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(other.result.outcome, AYNI_WRITE_OK);
    host->stop();
    host->start();
    requests.reset();
    StorageRequest &whole = requests.read_snapshot(0, 0, 8);
    StorageRequest &tail = requests.read_snapshot(0, 3, 8);
    StorageRequest &slot_one = requests.read_snapshot(1, 0, 8);
    requests.finish();
    EXPECT_EQ(whole.received, (Bytes{1, 2, 3, 4, 5}));
    EXPECT_EQ(tail.received, (Bytes{4, 5}));
    EXPECT_EQ(slot_one.received, (Bytes{8, 9}));
    StorageRequest &gap = requests.write_snapshot(1, 0, 9, {10});
    requests.finish();
    EXPECT_EQ(gap.result.outcome, AYNI_WRITE_FAILED);
    StorageRequest &oversized = requests.write_snapshot(1, 0, 8U * 1024U * 1024U, {10});
    requests.finish();
    EXPECT_EQ(oversized.result.outcome, AYNI_WRITE_FAILED);
}

// C-STORE-007/C-STORE-011: reclaim only bytes before the durable activation
// batch. A crash/reopen or repeated trim retains that batch and its tail.
TEST_P(HostStorage, trimming_keeps_the_activation_and_tail_and_is_idempotent) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const Bytes first = identified_batch(1, 11, 1);
    const Bytes activation = identified_batch(1, 12, 2);
    const Bytes tail = identified_batch(1, 13, 3);
    for (const auto &item :
         std::vector<std::pair<uint64_t, Bytes>>{{11, first}, {12, activation}, {13, tail}}) {
        StorageRequest &append =
            requests.append_identity(1, item.first, item.first - 10U, item.second);
        requests.finish();
        ASSERT_EQ(append.result.outcome, AYNI_WRITE_OK);
    }
    StorageRequest &trim = requests.trim(1, 12, 2);
    requests.finish();
    ASSERT_EQ(trim.result.outcome, AYNI_WRITE_OK);
    StorageRequest &physical = requests.physical_read(0, 4096);
    requests.finish();
    EXPECT_EQ(physical.received, joined({activation, tail}));
    host->stop();
    host->start();
    requests.reset();
    StorageRequest &retry = requests.trim(1, 12, 2);
    requests.finish();
    EXPECT_EQ(retry.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(requests.read_now(1), joined({activation, tail}));
    StorageRequest &missing = requests.trim(1, 999, 2);
    requests.finish();
    EXPECT_EQ(missing.result.outcome, AYNI_WRITE_FAILED);
    EXPECT_EQ(requests.read_now(1), joined({activation, tail}));
}

// C-COMMIT-008: UNKNOWN can mean persisted or lost; identical retries must
// recover both cases without storing the transition twice.
TEST(SimStorageFaults, retry_after_unknown_persisted_or_lost_stores_one_batch) {
    using ayni::hosts::sim::StorageFault;
    using ayni::hosts::sim::StoragePort;
    for (StorageFault fault :
         {StorageFault::unknown_persisted, StorageFault::unknown_lost, StorageFault::short_write}) {
        ayni::test::SimStorage storage;
        StorageRequests requests(storage);
        const Bytes batch = identified_batch(1, 10, 1);
        storage.sim().inject_fault(StoragePort::append, fault);
        StorageRequest &first = requests.append_identity(1, 10, 1, batch);
        requests.finish();
        EXPECT_NE(first.result.outcome, AYNI_WRITE_OK);
        StorageRequest &retry = requests.append_identity(1, 10, 1, batch);
        requests.finish();
        EXPECT_EQ(retry.result.outcome, AYNI_WRITE_OK);
        EXPECT_EQ(requests.read_now(1), batch);
        ASSERT_EQ(storage.sim().log().size(), 1U);
        EXPECT_FALSE(storage.sim().log().front().torn);
    }
}

// C-HOST-019/C-COMMIT-010: deferred backend work owns its bytes after the
// completion, and a durable epoch fence prevents its eventual mutation.
TEST(SimStorageFaults, fence_rejects_an_owned_late_write_after_its_unknown_completion) {
    using ayni::hosts::sim::StorageFault;
    using ayni::hosts::sim::StoragePort;
    ayni::test::SimStorage storage;
    StorageRequests requests(storage);
    storage.sim().inject_fault(StoragePort::append, StorageFault::unknown_deferred);
    StorageRequest &append = requests.append_identity(1, 10, 1, identified_batch(1, 10, 1));
    requests.finish();
    ASSERT_EQ(append.result.outcome, AYNI_WRITE_UNKNOWN);
    ASSERT_TRUE(append.memory.empty()); // helper released the borrowed bytes
    StorageRequest &fence = requests.fence(2);
    requests.finish();
    ASSERT_EQ(fence.result.outcome, AYNI_FENCE_EPOCH_INSTALLED);
    EXPECT_EQ(storage.sim().complete_late_storage(), 0U);
    EXPECT_TRUE(storage.sim().log().empty());
    const uint32_t epoch = storage.sim().writer_epoch();
    storage.stop();
    storage.start();
    EXPECT_EQ(storage.sim().writer_epoch(), epoch);
}

// A retry torn before the original deferred bytes arrive must not leave
// an unreadable middle frame hiding the complete original or its tail.
TEST(SimStorageFaults, late_complete_copy_replaces_a_torn_retry_before_later_batches) {
    using ayni::hosts::sim::StorageFault;
    using ayni::hosts::sim::StoragePort;
    ayni::test::SimStorage storage;
    StorageRequests requests(storage);
    const Bytes batch = identified_batch(1, 10, 1);
    const Bytes tail = identified_batch(1, 11, 2);
    storage.sim().inject_fault(StoragePort::append, StorageFault::unknown_deferred);
    StorageRequest &deferred = requests.append_identity(1, 10, 1, batch);
    requests.finish();
    ASSERT_EQ(deferred.result.outcome, AYNI_WRITE_UNKNOWN);
    storage.sim().inject_fault(StoragePort::append, StorageFault::short_write);
    StorageRequest &torn = requests.append_identity(1, 10, 1, batch);
    requests.finish();
    ASSERT_EQ(torn.result.outcome, AYNI_WRITE_FAILED);
    ASSERT_EQ(storage.sim().complete_late_storage(), 1U);
    ASSERT_EQ(storage.sim().log().size(), 1U);
    EXPECT_FALSE(storage.sim().log().front().torn);
    StorageRequest &next = requests.append_identity(1, 11, 2, tail);
    requests.finish();
    ASSERT_EQ(next.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(requests.read_now(1), joined({batch, tail}));
}

TEST(SimStorageFaults, a_torn_snapshot_chunk_can_be_retried_without_touching_the_other_slot) {
    using ayni::hosts::sim::StorageFault;
    using ayni::hosts::sim::StoragePort;
    ayni::test::SimStorage storage;
    StorageRequests requests(storage);
    StorageRequest &old = requests.write_snapshot(1, 1, 0, {8, 9});
    requests.finish();
    ASSERT_EQ(old.result.outcome, AYNI_WRITE_OK);
    storage.sim().inject_fault(StoragePort::write_snapshot, StorageFault::short_write);
    StorageRequest &torn = requests.write_snapshot(1, 0, 0, {1, 2, 3, 4, 5, 6});
    requests.finish();
    EXPECT_EQ(torn.result.outcome, AYNI_WRITE_FAILED);
    StorageRequest &retry = requests.write_snapshot(1, 0, 0, {1, 2, 3, 4, 5, 6});
    requests.finish();
    EXPECT_EQ(retry.result.outcome, AYNI_WRITE_OK);
    StorageRequest &candidate = requests.read_snapshot(0, 0, 8);
    StorageRequest &prior = requests.read_snapshot(1, 0, 8);
    requests.finish();
    EXPECT_EQ(candidate.received, (Bytes{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(prior.received, (Bytes{8, 9}));
}

TEST(SimStorageFaults, cached_free_space_accounts_for_pending_writes_and_exhaustion) {
    ayni::test::SimStorage storage;
    storage.sim().set_storage_capacity(100);
    StorageRequests requests(storage);
    EXPECT_EQ(requests.free_bytes(), 92U); // the framing header consumes 8
    StorageRequest &first = requests.append(1, test_batch(1, 60));
    EXPECT_EQ(requests.free_bytes(), 8U);
    requests.finish();
    ASSERT_EQ(first.result.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(requests.free_bytes(), 8U);
    StorageRequest &full = requests.append(2, test_batch(2, 10));
    requests.finish();
    EXPECT_EQ(full.result.outcome, AYNI_WRITE_FAILED);
    EXPECT_EQ(requests.read_now(1), test_batch(1, 60));
}

// C-HOST-004, C-HOST-006, C-HOST-018: several requests in flight at once,
// each answered once with the attempt it was given. Appends keep their
// order; a read in flight with them returns whole batches in log order.
TEST_P(HostStorage, every_accepted_request_completes_once_with_its_own_attempt) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const std::vector<Bytes> batches{test_batch(1, 40), test_batch(2, 50), test_batch(3, 60)};
    std::vector<StorageRequest *> issued;
    issued.push_back(&requests.append(1, batches[0]));
    StorageRequest &first_read = requests.read(1, 1024);
    issued.push_back(&first_read);
    issued.push_back(&requests.append(2, batches[1]));
    issued.push_back(&requests.append(3, batches[2]));
    StorageRequest &second_read = requests.read(1, 1024);
    issued.push_back(&second_read);
    requests.finish();

    std::set<uint64_t> request_ids;
    for (const StorageRequest *request : issued) {
        EXPECT_EQ(request->issued, IssueResult::in_flight);
        EXPECT_EQ(request->applied, 1);
        EXPECT_TRUE(same_attempt(request->result.attempt, request->attempt));
        EXPECT_EQ(request->result.outcome, AYNI_WRITE_OK);
        request_ids.insert(request->attempt.request_id);
    }
    EXPECT_EQ(request_ids.size(), issued.size());
    EXPECT_TRUE(whole_batch_prefix(first_read.received, batches));
    EXPECT_TRUE(whole_batch_prefix(second_read.received, batches));
    EXPECT_EQ(requests.read_now(1), joined({batches[0], batches[1], batches[2]}));
    EXPECT_EQ(requests.pool().counters().ignored_posts, 0U);
    EXPECT_EQ(requests.pool().counters().host_contract_violations, 0U);
}

TEST_P(HostStorage, batches_survive_a_new_host_instance) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const std::vector<Bytes> batches{test_batch(1, 40), test_batch(2, 50), test_batch(3, 60)};
    ASSERT_TRUE(requests.append_now(1, batches[0]));
    ASSERT_TRUE(requests.append_now(2, batches[1]));

    host->stop();
    host->start();
    requests.reset();
    EXPECT_EQ(requests.read_now(1), joined({batches[0], batches[1]}));
    ASSERT_TRUE(requests.append_now(3, batches[2]));

    host->stop();
    host->start();
    requests.reset();
    EXPECT_EQ(requests.read_now(1), joined({batches[0], batches[1], batches[2]}));
}

// C-HOST-009, C-STORE-002: whatever a power cut leaves of the batch being
// appended, the batches before it read back whole, the torn one is gone,
// and the next append takes its place after them.
TEST_P(HostStorage, torn_tail_is_dropped_and_overwritten) {
    const Bytes first = test_batch(1, 40);
    const Bytes second = test_batch(2, 50);
    const Bytes torn = test_batch(3, 24);
    const Bytes replacement = test_batch(4, 30);
    std::size_t cuts = 0;
    for (std::size_t way = 0;; ++way) {
        SCOPED_TRACE("power cut, way " + std::to_string(way));
        const std::unique_ptr<StorageHost> host = make_host();
        StorageRequests requests(*host);
        ASSERT_TRUE(requests.append_now(1, first));
        ASSERT_TRUE(requests.append_now(2, second));
        static_cast<void>(requests.append(3, torn));
        if (!host->cut_power_in_last_append(way)) {
            break;
        }
        ++cuts;
        host->start();
        requests.reset();
        EXPECT_EQ(requests.read_now(1), joined({first, second}));
        EXPECT_EQ(requests.read_now(3), Bytes{});

        ASSERT_TRUE(requests.append_now(3, replacement));
        EXPECT_EQ(requests.read_now(1), joined({first, second, replacement}));
        host->stop();
        host->start();
        requests.reset();
        EXPECT_EQ(requests.read_now(1), joined({first, second, replacement}));
        if (HasFailure()) {
            break;
        }
    }
    EXPECT_GT(cuts, 0U);
}

// C-HOST-009, C-STORE-009: a batch damaged on the medium with another one
// behind it is never skipped. A Host that checks its frames fails the read
// that reaches it; one that cannot returns it as stored, and the kernel's
// own checks catch it.
TEST_P(HostStorage, damaged_batch_before_the_last_is_never_dropped) {
    const std::vector<Bytes> batches{test_batch(1, 40), test_batch(2, 30), test_batch(3, 20)};
    std::size_t damaged = 0;
    for (std::size_t offset = 0;; ++offset) {
        SCOPED_TRACE("damaged byte: " + std::to_string(offset));
        const std::unique_ptr<StorageHost> host = make_host();
        StorageRequests requests(*host);
        for (uint64_t seq = 1; seq <= 3; ++seq) {
            ASSERT_TRUE(requests.append_now(seq, batches[seq - 1U]));
        }
        host->stop();
        if (!host->damage_batch(1, offset)) {
            break;
        }
        ++damaged;
        host->start();
        requests.reset();

        const std::optional<Bytes> all = requests.read_now(1);
        if (host->checks_frames()) {
            // Either the read fails at once, or it ends before the damage
            // and the next one fails there.
            if (all.has_value()) {
                EXPECT_EQ(*all, batches[0]);
                EXPECT_EQ(requests.read_now(2), std::nullopt);
            }
        } else {
            ASSERT_TRUE(all.has_value());
            EXPECT_EQ(all->size(), joined({batches[0], batches[1], batches[2]}).size());
            EXPECT_NE(*all, joined({batches[0], batches[1], batches[2]}));
        }
        if (HasFailure()) {
            break;
        }
    }
    EXPECT_GT(damaged, 0U);
}

// C-ID-006: a new Host instance answers only its own requests. The earlier
// instance ends with two appends in flight, and the new instance's requests
// reuse their slots and their attempts; each must get its own result, a
// read's, never the earlier append's.
TEST_P(HostStorage, new_host_instance_never_posts_an_earlier_request) {
    const std::unique_ptr<StorageHost> host = make_host();
    StorageRequests requests(*host);
    const std::vector<Bytes> batches{test_batch(1, 40), test_batch(2, 50), test_batch(3, 60)};
    ASSERT_TRUE(requests.append_now(1, batches[0]));
    StorageRequest &old_append = requests.append(2, batches[1]);
    const ayni_attempt_ref_v1 old_attempt = old_append.attempt;
    static_cast<void>(requests.append(3, batches[2]));
    host->stop();

    host->start();
    requests.reset();
    std::vector<StorageRequest *> reads;
    for (int read = 0; read < 3; ++read) {
        reads.push_back(&requests.read(1, 1024));
    }
    EXPECT_TRUE(same_attempt(reads[1]->attempt, old_attempt));
    requests.finish();
    for (const StorageRequest *read : reads) {
        EXPECT_EQ(read->applied, 1);
        EXPECT_EQ(read->result.outcome, AYNI_WRITE_OK);
        ASSERT_GE(read->received.size(), batches[0].size());
        EXPECT_TRUE(whole_batch_prefix(read->received, batches));
    }
    EXPECT_EQ(requests.pool().counters().ignored_posts, 0U);
    EXPECT_EQ(requests.pool().counters().host_contract_violations, 0U);
}

// A kernel on one Host instance, with the small capacities of the kernel
// tests. shut_down() ends the Host instance too, before ayni_destroy, as a
// composition root must (C-HOST-018).
class KernelRun {
  public:
    explicit KernelRun(StorageHost &host) : host_(host) {
        config_.struct_size = sizeof(config_);
        config_.abi_major = AYNI_ABI_MAJOR;
        config_.abi_minor = AYNI_ABI_MINOR;
        config_.ingress_capacity = 2;
        config_.event_body_max_bytes = 8;
        config_.completion_slot_count = 4;
        config_.storage_buffer_bytes = AYNI_STORAGE_BUFFER_BYTES_DEFAULT;
        config_.agent_capacity = 1;
        config_.goal_type_capacity = 1;
        config_.goal_capacity = 1;
        config_.goal_parameter_max_bytes = 8;
        config_.session_capacity = 1;
        config_.goal_depth_max = 0;
        config_.goal_children_max = 0;
        config_.task_capacity = 1;
        config_.strategy_capacity = 1;
        config_.policy_intent_capacity = 1;
        config_.task_step_capacity = 1;
        config_.task_checkpoint_max_bytes = 0;
        config_.command_capacity = 1;
        config_.capability_capacity = AYNI_CAPABILITY_CAPACITY_DEFAULT;
        config_.authority_scope_max_bytes = AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT;
    }

    KernelRun(const KernelRun &) = delete;
    KernelRun &operator=(const KernelRun &) = delete;

    ~KernelRun() {
        if (kernel_ != nullptr) {
            shut_down();
        }
    }

    // Creates and starts the kernel and steps it until recovery has an
    // outcome.
    void boot() {
        std::size_t size = 0;
        std::size_t align = 0;
        ASSERT_EQ(ayni_kernel_memory_requirements(&config_, &size, &align), AYNI_OK);
        ASSERT_LE(align, alignof(uint64_t));
        memory_.assign(size / sizeof(uint64_t) + 1U, 0);
        table_ = host_.table();
        ASSERT_EQ(ayni_create(&config_, &table_, memory_.data(), size, &kernel_), AYNI_OK);
        ASSERT_EQ(ayni_start(kernel_), AYNI_OK);
        step_while(AYNI_LIFECYCLE_RECOVERING);
    }

    void shut_down() {
        ASSERT_EQ(ayni_begin_shutdown(kernel_), AYNI_OK);
        step_while(AYNI_LIFECYCLE_DRAINING);
        ASSERT_EQ(lifecycle(), AYNI_LIFECYCLE_STOPPED);
        host_.stop();
        ASSERT_EQ(ayni_destroy(kernel_), AYNI_OK);
        kernel_ = nullptr;
    }

    ayni_lifecycle_t lifecycle() const {
        ayni_lifecycle_t state = 0;
        EXPECT_EQ(ayni_get_lifecycle(kernel_, &state), AYNI_OK);
        return state;
    }

    ayni_kernel_info_v1 info() const {
        ayni_kernel_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_get_kernel_info(kernel_, &out), AYNI_OK);
        return out;
    }

  private:
    void step_while(ayni_lifecycle_t state) {
        for (int round = 0; round < 100 && lifecycle() == state; ++round) {
            ayni_step_report_v1 report{};
            report.struct_size = sizeof(report);
            ASSERT_EQ(ayni_step(kernel_, 1000, 1000000, &report), AYNI_OK);
            if (report.more_ready == 0U && lifecycle() == state) {
                host_.serve(report.next_deadline_us);
            }
        }
        EXPECT_NE(lifecycle(), state);
    }

    StorageHost &host_;
    ayni_config_v1 config_{};
    ayni_host_v1 table_{};
    std::vector<uint64_t> memory_;
    ayni_kernel_t *kernel_ = nullptr;
};

// C-ID-009 on each Host: every boot reads what the earlier ones stored and
// persists the next epoch.
TEST_P(HostStorage, kernel_boots_on_each_new_host_instance) {
    const std::unique_ptr<StorageHost> host = make_host();
    for (uint64_t epoch = 1; epoch <= 3; ++epoch) {
        KernelRun kernel(*host);
        kernel.boot();
        EXPECT_EQ(kernel.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        EXPECT_EQ(kernel.info().boot_epoch, epoch);
        kernel.shut_down();
        host->start();
    }
}

// C-STORE-009: a damaged boot batch with another behind it restricts the
// kernel, through a failed read or through the kernel's own check.
TEST_P(HostStorage, damaged_batch_restricts_the_kernel) {
    const std::unique_ptr<StorageHost> host = make_host();
    for (int boot = 0; boot < 3; ++boot) {
        KernelRun kernel(*host);
        kernel.boot();
        ASSERT_EQ(kernel.lifecycle(), AYNI_LIFECYCLE_RUNNING);
        kernel.shut_down();
        host->start();
    }
    host->stop();
    // Byte 24 is inside the second batch on both Hosts: its transition_id,
    // or the native frame's payload.
    ASSERT_TRUE(host->damage_batch(1, 24));
    host->start();

    KernelRun kernel(*host);
    kernel.boot();
    EXPECT_EQ(kernel.lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(kernel.info().restriction, host->checks_frames()
                                             ? AYNI_RESTRICTION_STORAGE_READ_FAILED
                                             : AYNI_RESTRICTION_LOG_CORRUPT);
    EXPECT_EQ(kernel.info().boot_epoch, 0U);
}

} // namespace
