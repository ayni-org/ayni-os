// Native Host services (C-HOST-007, ADR-0015): clock, wake and ingress lock;
// and what is particular to its file storage (MA-010): opening, the file
// format, truncating a torn tail, refusing appends to a damaged log, and
// closing. Behavior shared with the sim Host is in
// host_storage_contract_test.cpp.

#include "../../hosts/native/file_log.h"
#include "durability/batch/batch_codec.h"
#include "foundation/crc32.h"
#include "storage_hosts.h"
#include "temp_directory.h"

#include <ayni/hosts/native.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace ayni::hosts::native {

// ADR-0039: inject only the OS space observation; exercise the real worker,
// cache, wake and close paths without changing the shared storage contract.
class HostTestPeer {
  public:
    static StorageOpen open(Host &host, const char *directory, void *context,
                            bool (*read)(void *, uint64_t &)) {
        return host.open_storage(directory, 1, {context, read});
    }
};

} // namespace ayni::hosts::native

namespace {

using namespace std::chrono_literals;
using ayni::hosts::native::FileLog;
using ayni::hosts::native::Host;
using ayni::hosts::native::StorageStatus;
using ayni::kernel::IssueResult;
using ayni::test::Bytes;
using ayni::test::joined;
using ayni::test::NativeFileStorage;
using ayni::test::StorageRequest;
using ayni::test::StorageRequests;
using ayni::test::TempDirectory;
using ayni::test::test_batch;

// ADR-0104: a bounded consecutive group writes complete frames and one final durable prefix.
TEST(NativeHost, grouped_frames_preserve_exact_bytes_through_normal_close_and_reopen) {
    TempDirectory directory;
    ayni::hosts::native::PosixFiles files;
    ayni::hosts::storage::FileLog log(files);
    ASSERT_EQ(log.open(directory.path().c_str(), "commit_log").status, FileLog::OpenStatus::ok);
    const auto first = test_batch(1, 40), second = test_batch(2, 50);
    const std::array<ayni_bytes_v1, 2> group{
        {{first.data(), static_cast<uint32_t>(first.size())},
         {second.data(), static_cast<uint32_t>(second.size())}}};
    ASSERT_EQ(log.append_many(1, group.data(), static_cast<uint32_t>(group.size())).outcome,
              AYNI_WRITE_OK);
    log.close();
    ASSERT_EQ(log.open(directory.path().c_str(), "commit_log").status, FileLog::OpenStatus::ok);
    std::vector<uint8_t> bytes(first.size() + second.size());
    const auto read = log.read_prefix(1, bytes.data(), static_cast<uint32_t>(bytes.size()));
    ASSERT_EQ(read.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(bytes, joined({first, second}));
}

// A failed observation is distinct from a valid zero-byte observation.
// ADR-0086 / C-STORE-004: reject an old native prefix before new outer-store creation.
TEST(NativeHost, unsupported_snapshot_schema_is_preserved_before_log_creation) {
    const TempDirectory directory;
    const std::array<uint8_t, AYNI_STORAGE_SCHEMA_PREFIX_BYTES> old{
        {'A', 'Y', 'S', 'N', AYNI_STORAGE_SCHEMA_CURRENT - 1, 0}};
    const auto path = directory.file("snapshot_0");
    {
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char *>(old.data()),
                   static_cast<std::streamsize>(old.size()));
    }
    Host host;
    EXPECT_EQ(host.open_storage(directory.path().c_str(), 1).status, StorageStatus::not_a_log);
    EXPECT_FALSE(std::filesystem::exists(directory.file(Host::log_file_name)));
    std::ifstream file(path, std::ios::binary);
    const Bytes after{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_EQ(after, Bytes(old.begin(), old.end()));
}

struct SpaceProbe {
    std::atomic<uint64_t> available{0};
    std::atomic<unsigned> samples{0};

    static bool read(void *context, uint64_t &out) {
        auto &probe = *static_cast<SpaceProbe *>(context);
        out = probe.available.load();
        ++probe.samples;
        return out != UINT64_MAX;
    }
};

// C-HOST-013 / ADR-0039: no queued I/O is needed to recover admission or
// notify the owner; free_bytes continues to read the worker's cached result.
TEST(NativeHost, idle_space_recovery_wakes_the_owner_without_a_storage_request) {
    const TempDirectory directory;
    SpaceProbe probe;
    Host host; // Destroy the worker before its borrowed probe.
    ASSERT_EQ(ayni::hosts::native::HostTestPeer::open(host, directory.path().c_str(), &probe,
                                                      &SpaceProbe::read)
                  .status,
              StorageStatus::ok);
    const auto ports = host.table();
    uint64_t available = UINT64_MAX;
    ASSERT_EQ(ports.storage_free_bytes(ports.ctx, &available), AYNI_OK);
    ASSERT_EQ(available, 0U);
    probe.available = 4096;
    ASSERT_TRUE(host.wait_for_wake(5s)); // Watchdog, not a filesystem latency promise.
    ASSERT_EQ(ports.storage_free_bytes(ports.ctx, &available), AYNI_OK);
    EXPECT_EQ(available, 4096U);
    const auto before = probe.samples.load();
    EXPECT_FALSE(host.wait_for_wake(1500ms));
    host.close_storage(); // Joins the idle worker and prevents further samples.
    EXPECT_GT(probe.samples.load(), before);
    EXPECT_FALSE(host.wait_for_wake(0us));
}

// C-HOST-013 / ADR-0039: both loss of observability and recovery are published;
// an unsuccessful sample never disables future idle observations.
TEST(NativeHost, idle_space_probe_retries_failures_and_reports_each_status_change) {
    const TempDirectory directory;
    SpaceProbe probe;
    probe.available = UINT64_MAX;
    Host host;
    ASSERT_EQ(ayni::hosts::native::HostTestPeer::open(host, directory.path().c_str(), &probe,
                                                      &SpaceProbe::read)
                  .status,
              StorageStatus::ok);
    const auto ports = host.table();
    uint64_t available = 1;
    EXPECT_EQ(ports.storage_free_bytes(ports.ctx, &available), AYNI_STORAGE_FAILED);
    EXPECT_EQ(available, 0U);
    probe.available = 8192;
    ASSERT_TRUE(host.wait_for_wake(5s));
    EXPECT_EQ(ports.storage_free_bytes(ports.ctx, &available), AYNI_OK);
    EXPECT_EQ(available, 8192U);
    probe.available = UINT64_MAX;
    ASSERT_TRUE(host.wait_for_wake(5s));
    EXPECT_EQ(ports.storage_free_bytes(ports.ctx, &available), AYNI_STORAGE_FAILED);
    EXPECT_EQ(available, 0U);
    probe.available = 0;
    ASSERT_TRUE(host.wait_for_wake(5s));
    EXPECT_EQ(ports.storage_free_bytes(ports.ctx, &available), AYNI_OK);
    EXPECT_EQ(available, 0U);
}

void write_file(const std::string &path, const Bytes &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

Bytes le32(uint32_t value) {
    return {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U),
            static_cast<uint8_t>(value >> 16U), static_cast<uint8_t>(value >> 24U)};
}

// Real files with deterministic failures at the mutation boundary. A sync
// can return EIO while its complete bytes remain readable, as cached pages
// do after writeback errors; writes are recorded to prove they are redirtied.
struct FileIoProbe {
    struct Write {
        uint64_t offset;
        Bytes bytes;
    };
    std::vector<Write> writes;
    int fail_file_sync = 0;
    int fail_directory_sync = 0;
    bool tear_next_write = false;
    int file_syncs = 0;
    int directory_syncs = 0;

    FileLog::Io io() {
        return {this, &FileIoProbe::write, &FileIoProbe::sync_file, &FileIoProbe::sync_directory};
    }

    static int write(void *context, int fd, uint64_t offset, const uint8_t *data,
                     std::size_t size) {
        auto &probe = *static_cast<FileIoProbe *>(context);
        const bool torn = std::exchange(probe.tear_next_write, false);
        const std::size_t count = torn ? size / 2U : size;
        probe.writes.push_back({offset, Bytes(data, data + count)});
        std::size_t done = 0;
        while (done < count) {
            const ssize_t put =
                ::pwrite(fd, data + done, count - done, static_cast<off_t>(offset + done));
            if (put < 0 && errno == EINTR) {
                continue;
            }
            if (put <= 0) {
                return put == 0 ? EIO : errno;
            }
            done += static_cast<std::size_t>(put);
        }
        return torn ? EIO : 0;
    }

    static int sync_file(void *context, int fd) {
        auto &probe = *static_cast<FileIoProbe *>(context);
        ++probe.file_syncs;
        if (::fdatasync(fd) != 0) {
            return errno;
        }
        if (probe.fail_file_sync > 0) {
            --probe.fail_file_sync;
            return EIO;
        }
        return 0;
    }

    static int sync_directory(void *context, int fd) {
        auto &probe = *static_cast<FileIoProbe *>(context);
        ++probe.directory_syncs;
        if (::fsync(fd) != 0) {
            return errno;
        }
        if (probe.fail_directory_sync > 0) {
            --probe.fail_directory_sync;
            return EIO;
        }
        return 0;
    }
};

TEST(NativeHost, unknown_log_sync_rewrites_the_entire_frame_before_a_successful_retry) {
    const TempDirectory directory;
    FileIoProbe probe;
    FileLog log(probe.io());
    ASSERT_EQ(log.open(directory.path().c_str(), Host::log_file_name).status,
              FileLog::OpenStatus::ok);
    const Bytes first = test_batch(1, 30);
    const Bytes uncertain = test_batch(2, 50);
    ASSERT_EQ(log.append(1, first.data(), static_cast<uint32_t>(first.size())).outcome,
              AYNI_WRITE_OK);
    probe.fail_file_sync = 2;
    ASSERT_EQ(log.append(2, uncertain.data(), static_cast<uint32_t>(uncertain.size())).outcome,
              AYNI_WRITE_UNKNOWN);
    probe.writes.clear();
    EXPECT_EQ(log.append(2, uncertain.data(), static_cast<uint32_t>(uncertain.size())).outcome,
              AYNI_WRITE_UNKNOWN);
    ASSERT_EQ(probe.writes.size(), 1U);
    EXPECT_EQ(probe.writes.front().offset, 8U + 24U + first.size());
    EXPECT_EQ(probe.writes.front().bytes.size(), 24U + uncertain.size());
    EXPECT_EQ(Bytes(probe.writes.front().bytes.begin() + 20, probe.writes.front().bytes.end() - 4),
              uncertain);
    Bytes prefix(256);
    const auto still_uncertain =
        log.read_log(0, prefix.data(), static_cast<uint32_t>(prefix.size()));
    ASSERT_EQ(still_uncertain.outcome, AYNI_WRITE_OK);
    prefix.resize(static_cast<std::size_t>(still_uncertain.value));
    EXPECT_EQ(prefix, first);
    probe.writes.clear();
    EXPECT_EQ(log.append(2, uncertain.data(), static_cast<uint32_t>(uncertain.size())).outcome,
              AYNI_WRITE_OK);
    ASSERT_EQ(probe.writes.size(), 1U);
    Bytes read(256);
    const FileLog::Result result = log.read_log(0, read.data(), static_cast<uint32_t>(read.size()));
    ASSERT_EQ(result.outcome, AYNI_WRITE_OK);
    read.resize(static_cast<std::size_t>(result.value));
    EXPECT_EQ(read, joined({first, uncertain}));
}

// Closing a process loses its in-memory EIO marker, not the requirement to
// redirty inherited cached bytes before acknowledging any durable mutation.
TEST(NativeHost, reopened_unknown_batch_is_rewritten_before_an_identical_retry_succeeds) {
    const TempDirectory directory;
    FileIoProbe probe;
    const Bytes batch = test_batch(1, 30);
    {
        FileLog first(probe.io());
        ASSERT_EQ(first.open(directory.path().c_str(), Host::log_file_name).status,
                  FileLog::OpenStatus::ok);
        probe.fail_file_sync = 1;
        ASSERT_EQ(first.append(1, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
                  AYNI_WRITE_UNKNOWN);
    }
    probe.writes.clear();
    FileLog reopened(probe.io());
    ASSERT_EQ(reopened.open(directory.path().c_str(), Host::log_file_name).status,
              FileLog::OpenStatus::ok);
    Bytes read(128);
    const auto inherited = reopened.read_log(0, read.data(), static_cast<uint32_t>(read.size()));
    ASSERT_EQ(inherited.outcome, AYNI_WRITE_OK);
    read.resize(static_cast<std::size_t>(inherited.value));
    ASSERT_EQ(read, batch);
    EXPECT_TRUE(probe.writes.empty()); // open and recovery reads never repair blindly
    const int initial_syncs = probe.file_syncs;
    probe.fail_file_sync = 1;
    EXPECT_EQ(reopened.append(1, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
              AYNI_WRITE_UNKNOWN);
    ASSERT_EQ(probe.writes.size(), 1U);
    EXPECT_EQ(probe.writes.front().offset, 0U);
    EXPECT_EQ(probe.writes.front().bytes.size(), 8U + 24U + batch.size());
    EXPECT_EQ(probe.file_syncs, initial_syncs + 1);
    probe.writes.clear();
    EXPECT_EQ(reopened.append(1, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
              AYNI_WRITE_OK);
    ASSERT_EQ(probe.writes.size(), 1U);
    EXPECT_EQ(probe.writes.front().offset, 0U);
    EXPECT_EQ(probe.file_syncs, initial_syncs + 2);
    read.assign(128, 0);
    const auto complete = reopened.read_log(0, read.data(), static_cast<uint32_t>(read.size()));
    ASSERT_EQ(complete.outcome, AYNI_WRITE_OK);
    read.resize(static_cast<std::size_t>(complete.value));
    EXPECT_EQ(read, batch);
}

TEST(NativeHost, first_new_append_cannot_bypass_an_unknown_prefix_from_an_earlier_process) {
    const TempDirectory directory;
    FileIoProbe probe;
    const Bytes previous = test_batch(1, 30);
    const Bytes next = test_batch(2, 40);
    {
        FileLog first(probe.io());
        ASSERT_EQ(first.open(directory.path().c_str(), Host::log_file_name).status,
                  FileLog::OpenStatus::ok);
        probe.fail_file_sync = 1;
        ASSERT_EQ(first.append(1, previous.data(), static_cast<uint32_t>(previous.size())).outcome,
                  AYNI_WRITE_UNKNOWN);
    }
    probe.writes.clear();
    FileLog reopened(probe.io());
    ASSERT_EQ(reopened.open(directory.path().c_str(), Host::log_file_name).status,
              FileLog::OpenStatus::ok);
    ASSERT_TRUE(probe.writes.empty());
    probe.fail_file_sync = 1;
    EXPECT_EQ(reopened.append(2, next.data(), static_cast<uint32_t>(next.size())).outcome,
              AYNI_WRITE_UNKNOWN);
    ASSERT_EQ(probe.writes.size(), 1U);
    EXPECT_EQ(probe.writes.front().offset, 0U);
    EXPECT_EQ(probe.writes.front().bytes.size(), 8U + 24U + previous.size());
    probe.writes.clear();
    EXPECT_EQ(reopened.append(2, next.data(), static_cast<uint32_t>(next.size())).outcome,
              AYNI_WRITE_OK);
    ASSERT_EQ(probe.writes.size(), 4U); // inherited prefix, then the new frame's three parts
    EXPECT_EQ(probe.writes.front().offset, 0U);
    EXPECT_EQ(probe.writes[1].offset, 8U + 24U + previous.size());
    Bytes read(256);
    const auto result = reopened.read_log(0, read.data(), static_cast<uint32_t>(read.size()));
    ASSERT_EQ(result.outcome, AYNI_WRITE_OK);
    read.resize(static_cast<std::size_t>(result.value));
    EXPECT_EQ(read, joined({previous, next}));
}

TEST(NativeHost, fence_snapshot_and_trim_confirm_the_inherited_prefix_before_mutating) {
    for (int operation = 0; operation < 3; ++operation) {
        SCOPED_TRACE(operation);
        const TempDirectory directory;
        FileIoProbe probe;
        Bytes batch(128);
        ayni::durability::BatchWriter writer(batch.data(), batch.size(), {1, 1, 1});
        batch.resize(writer.finish());
        {
            FileLog first(probe.io());
            ASSERT_EQ(first.open(directory.path().c_str(), Host::log_file_name).status,
                      FileLog::OpenStatus::ok);
            probe.fail_file_sync = 1;
            ASSERT_EQ(first.append(1, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
                      AYNI_WRITE_UNKNOWN);
        }
        probe.writes.clear();
        FileLog reopened(probe.io());
        ASSERT_EQ(reopened.open(directory.path().c_str(), Host::log_file_name).status,
                  FileLog::OpenStatus::ok);
        ASSERT_TRUE(probe.writes.empty());
        const Bytes candidate{7, 8, 9};
        const auto mutate = [&] {
            if (operation == 0) {
                return reopened.fence(2);
            }
            if (operation == 1) {
                return reopened.write_snapshot(0, 1, 0, candidate.data(),
                                               static_cast<uint32_t>(candidate.size()));
            }
            return reopened.trim_prefix(1, 1, 1);
        };
        probe.fail_file_sync = 1;
        EXPECT_EQ(mutate().outcome, operation == 0 ? AYNI_FENCE_UNAVAILABLE : AYNI_WRITE_UNKNOWN);
        ASSERT_EQ(probe.writes.size(), 1U);
        EXPECT_EQ(probe.writes.front().offset, 0U);
        EXPECT_EQ(probe.writes.front().bytes.size(), 8U + 24U + batch.size());
        probe.writes.clear();
        EXPECT_EQ(mutate().outcome, operation == 0 ? AYNI_FENCE_DRAINED : AYNI_WRITE_OK);
        ASSERT_FALSE(probe.writes.empty());
        EXPECT_EQ(probe.writes.front().offset, 0U);
    }
}

TEST(NativeHost, unknown_snapshot_sync_rewrites_an_identical_chunk_on_each_retry) {
    const TempDirectory directory;
    FileIoProbe probe;
    FileLog log(probe.io());
    ASSERT_EQ(log.open(directory.path().c_str(), Host::log_file_name).status,
              FileLog::OpenStatus::ok);
    const Bytes chunk{1, 2, 3, 4, 5};
    probe.fail_file_sync = 2;
    ASSERT_EQ(
        log.write_snapshot(0, 1, 0, chunk.data(), static_cast<uint32_t>(chunk.size())).outcome,
        AYNI_WRITE_UNKNOWN);
    probe.writes.clear();
    EXPECT_EQ(
        log.write_snapshot(0, 1, 0, chunk.data(), static_cast<uint32_t>(chunk.size())).outcome,
        AYNI_WRITE_UNKNOWN);
    ASSERT_EQ(probe.writes.size(), 1U);
    EXPECT_EQ(probe.writes.front().offset, 0U);
    EXPECT_EQ(probe.writes.front().bytes, chunk);
    probe.writes.clear();
    EXPECT_EQ(
        log.write_snapshot(0, 1, 0, chunk.data(), static_cast<uint32_t>(chunk.size())).outcome,
        AYNI_WRITE_OK);
    ASSERT_EQ(probe.writes.size(), 1U);
    EXPECT_EQ(probe.writes.front().bytes, chunk);
}

TEST(NativeHost, retry_after_a_short_file_write_replaces_the_torn_tail) {
    const TempDirectory directory;
    FileIoProbe probe;
    FileLog log(probe.io());
    ASSERT_EQ(log.open(directory.path().c_str(), Host::log_file_name).status,
              FileLog::OpenStatus::ok);
    const Bytes batch = test_batch(1, 30);
    probe.tear_next_write = true;
    EXPECT_EQ(log.append(1, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
              AYNI_WRITE_FAILED);
    EXPECT_EQ(log.append(1, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
              AYNI_WRITE_OK);
    Bytes read(128);
    const auto result = log.read_log(0, read.data(), static_cast<uint32_t>(read.size()));
    ASSERT_EQ(result.outcome, AYNI_WRITE_OK);
    read.resize(static_cast<std::size_t>(result.value));
    EXPECT_EQ(read, batch);
}

TEST(NativeHost, trimming_retries_the_directory_sync_after_rename_was_unknown) {
    const TempDirectory directory;
    FileIoProbe probe;
    FileLog log(probe.io());
    ASSERT_EQ(log.open(directory.path().c_str(), Host::log_file_name).status,
              FileLog::OpenStatus::ok);
    for (uint64_t seq = 1; seq <= 2; ++seq) {
        Bytes batch(128);
        ayni::durability::BatchWriter writer(batch.data(), batch.size(), {1, seq, seq});
        batch.resize(writer.finish());
        ASSERT_EQ(log.append(seq, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
                  AYNI_WRITE_OK);
    }
    const int initial_syncs = probe.directory_syncs;
    probe.fail_directory_sync = 2;
    ASSERT_EQ(log.trim_prefix(1, 2, 2).outcome, AYNI_WRITE_UNKNOWN);
    EXPECT_EQ(log.trim_prefix(1, 2, 2).outcome, AYNI_WRITE_UNKNOWN);
    EXPECT_EQ(probe.directory_syncs, initial_syncs + 2);
    EXPECT_EQ(log.trim_prefix(1, 2, 2).outcome, AYNI_WRITE_OK);
    // Reconfirm file and directory, then finish the idempotent trim.
    EXPECT_EQ(probe.directory_syncs, initial_syncs + 4);
}

TEST(NativeHost, unknown_rotation_requires_directory_confirmation_before_append_or_fence) {
    for (bool reopen : {false, true}) {
        SCOPED_TRACE(reopen);
        for (bool use_fence : {false, true}) {
            SCOPED_TRACE(use_fence);
            const TempDirectory directory;
            FileIoProbe probe;
            FileLog first(probe.io());
            ASSERT_EQ(first.open(directory.path().c_str(), Host::log_file_name).status,
                      FileLog::OpenStatus::ok);
            Bytes activation;
            for (uint64_t seq = 1; seq <= 2; ++seq) {
                Bytes batch(128);
                ayni::durability::BatchWriter writer(batch.data(), batch.size(), {1, seq, seq});
                batch.resize(writer.finish());
                ASSERT_EQ(
                    first.append(seq, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
                    AYNI_WRITE_OK);
                activation = std::move(batch);
            }
            probe.fail_directory_sync = 1;
            ASSERT_EQ(first.trim_prefix(1, 2, 2).outcome, AYNI_WRITE_UNKNOWN);
            FileLog reopened(probe.io());
            FileLog *active = &first;
            probe.writes.clear();
            if (reopen) {
                first.close();
                ASSERT_EQ(reopened.open(directory.path().c_str(), Host::log_file_name).status,
                          FileLog::OpenStatus::ok);
                active = &reopened;
                EXPECT_TRUE(probe.writes.empty());
            }
            const Bytes next = test_batch(3, 30);
            const auto mutate = [&] {
                return use_fence
                           ? active->fence(2)
                           : active->append(3, next.data(), static_cast<uint32_t>(next.size()));
            };
            const int initial_syncs = probe.directory_syncs;
            probe.fail_directory_sync = 2;
            for (int attempt = 0; attempt < 2; ++attempt) {
                EXPECT_EQ(mutate().outcome,
                          use_fence ? AYNI_FENCE_UNAVAILABLE : AYNI_WRITE_UNKNOWN);
                ASSERT_EQ(probe.writes.size(), 1U);
                EXPECT_EQ(probe.writes.front().offset, 0U);
                EXPECT_EQ(probe.writes.front().bytes.size(), 8U + 24U + activation.size());
                EXPECT_EQ(probe.directory_syncs, initial_syncs + attempt + 1);
                probe.writes.clear();
            }
            EXPECT_EQ(mutate().outcome, use_fence ? AYNI_FENCE_DRAINED : AYNI_WRITE_OK);
            EXPECT_EQ(probe.directory_syncs, initial_syncs + 3);
            ASSERT_FALSE(probe.writes.empty());
            EXPECT_EQ(probe.writes.front().offset, 0U);
            Bytes read(256);
            const auto result =
                active->read_log(0, read.data(), static_cast<uint32_t>(read.size()));
            ASSERT_EQ(result.outcome, AYNI_WRITE_OK);
            read.resize(static_cast<std::size_t>(result.value));
            EXPECT_EQ(read, use_fence ? activation : joined({activation, next}));
        }
    }
}

TEST(NativeHost, reopened_header_creation_confirms_its_directory_before_new_work) {
    const TempDirectory directory;
    FileIoProbe probe;
    {
        FileLog first(probe.io());
        probe.fail_directory_sync = 1;
        const auto created = first.open(directory.path().c_str(), Host::log_file_name);
        ASSERT_EQ(created.status, FileLog::OpenStatus::file_error);
        EXPECT_EQ(created.error, EIO);
    }
    FileLog reopened(probe.io());
    probe.writes.clear();
    ASSERT_EQ(reopened.open(directory.path().c_str(), Host::log_file_name).status,
              FileLog::OpenStatus::ok);
    EXPECT_TRUE(probe.writes.empty());
    const int initial_syncs = probe.directory_syncs;
    const Bytes batch = test_batch(1, 30);
    probe.fail_directory_sync = 1;
    EXPECT_EQ(reopened.append(1, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
              AYNI_WRITE_UNKNOWN);
    ASSERT_EQ(probe.writes.size(), 1U);
    EXPECT_EQ(probe.writes.front().bytes.size(), 8U);
    EXPECT_EQ(probe.directory_syncs, initial_syncs + 1);
    probe.fail_directory_sync = 1;
    EXPECT_EQ(reopened.fence(2).outcome, AYNI_FENCE_UNAVAILABLE);
    EXPECT_EQ(probe.directory_syncs, initial_syncs + 2);
    EXPECT_EQ(reopened.append(1, batch.data(), static_cast<uint32_t>(batch.size())).outcome,
              AYNI_WRITE_OK);
    EXPECT_EQ(probe.directory_syncs, initial_syncs + 3);
    Bytes read(128);
    const auto result = reopened.read_log(0, read.data(), static_cast<uint32_t>(read.size()));
    ASSERT_EQ(result.outcome, AYNI_WRITE_OK);
    read.resize(static_cast<std::size_t>(result.value));
    EXPECT_EQ(read, batch);
}

TEST(NativeHost, table_is_complete_and_points_at_the_host) {
    ayni::hosts::native::Host native;
    const ayni_host_v1 host = native.table();
    EXPECT_EQ(host.struct_size, sizeof(ayni_host_v1));
    EXPECT_EQ(host.ctx, &native);
    EXPECT_NE(host.monotonic_now_us, nullptr);
    EXPECT_NE(host.wake, nullptr);
    EXPECT_NE(host.ingress_lock, nullptr);
    EXPECT_NE(host.ingress_unlock, nullptr);
}

TEST(NativeHost, monotonic_clock_never_decreases) {
    ayni::hosts::native::Host native;
    const ayni_host_v1 host = native.table();
    uint64_t previous = host.monotonic_now_us(host.ctx);
    for (int sample = 0; sample < 10000; ++sample) {
        const uint64_t now = host.monotonic_now_us(host.ctx);
        ASSERT_GE(now, previous);
        previous = now;
    }
}

TEST(NativeHost, wakes_from_other_threads_merge_into_one) {
    ayni::hosts::native::Host native;
    const ayni_host_v1 host = native.table();
    EXPECT_FALSE(native.wait_for_wake(1ms));

    std::vector<std::thread> wakers;
    for (int thread = 0; thread < 4; ++thread) {
        wakers.emplace_back([&host] { host.wake(host.ctx); });
    }
    for (std::thread &waker : wakers) {
        waker.join();
    }
    EXPECT_TRUE(native.wait_for_wake(1s));
    EXPECT_FALSE(native.wait_for_wake(1ms));
}

TEST(NativeHost, ingress_lock_excludes_other_threads) {
    ayni::hosts::native::Host native;
    const ayni_host_v1 host = native.table();
    uint64_t counter = 0; // unsynchronized except by the ingress lock
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&host, &counter] {
            for (int round = 0; round < 10000; ++round) {
                host.ingress_lock(host.ctx);
                ++counter;
                host.ingress_unlock(host.ctx);
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(counter, 40000U);
}

// C-HOST-015: without its log the Host offers no storage ports, so a kernel
// on it cannot start rather than run without a log.
TEST(NativeHost, table_has_storage_ports_only_while_storage_is_open) {
    const TempDirectory directory;
    Host native;
    EXPECT_EQ(native.table().storage_append, nullptr);
    EXPECT_EQ(native.table().storage_read_prefix, nullptr);
    EXPECT_EQ(native.table().storage_read_log, nullptr);
    EXPECT_EQ(native.table().storage_fence_writers, nullptr);
    EXPECT_EQ(native.table().storage_write_snapshot, nullptr);
    EXPECT_EQ(native.table().storage_read_snapshot, nullptr);
    EXPECT_EQ(native.table().storage_trim_prefix, nullptr);
    EXPECT_EQ(native.table().storage_free_bytes, nullptr);
    ASSERT_EQ(native.open_storage(directory.path().c_str(), 4).status, StorageStatus::ok);
    EXPECT_NE(native.table().storage_append, nullptr);
    EXPECT_NE(native.table().storage_read_prefix, nullptr);
    EXPECT_NE(native.table().storage_read_log, nullptr);
    EXPECT_NE(native.table().storage_fence_writers, nullptr);
    EXPECT_NE(native.table().storage_write_snapshot, nullptr);
    EXPECT_NE(native.table().storage_read_snapshot, nullptr);
    EXPECT_NE(native.table().storage_trim_prefix, nullptr);
    EXPECT_NE(native.table().storage_free_bytes, nullptr);
    native.close_storage();
    EXPECT_EQ(native.table().storage_append, nullptr);
    EXPECT_EQ(native.table().storage_read_prefix, nullptr);
    EXPECT_EQ(native.table().storage_read_log, nullptr);
    EXPECT_EQ(native.table().storage_fence_writers, nullptr);
    EXPECT_EQ(native.table().storage_write_snapshot, nullptr);
    EXPECT_EQ(native.table().storage_read_snapshot, nullptr);
    EXPECT_EQ(native.table().storage_trim_prefix, nullptr);
    EXPECT_EQ(native.table().storage_free_bytes, nullptr);
}

TEST(NativeHost, open_storage_checks_its_arguments_and_opens_once) {
    const TempDirectory directory;
    Host native;
    EXPECT_EQ(native.open_storage(nullptr, 4).status, StorageStatus::invalid_argument);
    EXPECT_EQ(native.open_storage(directory.path().c_str(), 0).status,
              StorageStatus::invalid_argument);
    EXPECT_EQ(native.open_storage(directory.path().c_str(), 4).status, StorageStatus::ok);
    EXPECT_EQ(native.open_storage(directory.path().c_str(), 4).status, StorageStatus::already_open);
}

// One writer per log, or two Hosts would interleave their appends.
TEST(NativeHost, second_host_on_the_same_log_is_in_use) {
    const TempDirectory directory;
    Host first;
    Host second;
    ASSERT_EQ(first.open_storage(directory.path().c_str(), 4).status, StorageStatus::ok);
    EXPECT_EQ(second.open_storage(directory.path().c_str(), 4).status, StorageStatus::in_use);
    first.close_storage();
    EXPECT_EQ(second.open_storage(directory.path().c_str(), 4).status, StorageStatus::ok);
}

// The directory lock survives inode replacement, and the replacement log
// is locked too so an older Host using only the file lock stays excluded.
TEST(NativeHost, rotation_keeps_the_directory_and_replacement_log_exclusive) {
    NativeFileStorage storage;
    StorageRequests requests(storage);
    for (uint64_t seq = 1; seq <= 2; ++seq) {
        Bytes encoded(128);
        ayni::durability::BatchWriter writer(encoded.data(), encoded.size(), {1, seq, seq});
        encoded.resize(writer.finish());
        ASSERT_TRUE(requests.append_now(seq, encoded));
    }
    StorageRequest &trim = requests.trim(1, 2, 2);
    requests.finish();
    ASSERT_EQ(trim.result.outcome, AYNI_WRITE_OK);
    Host contender;
    const std::string directory = std::filesystem::path(storage.log_path()).parent_path().string();
    EXPECT_EQ(contender.open_storage(directory.c_str(), 4).status, StorageStatus::in_use);
    const int replacement = ::open(storage.log_path().c_str(), O_RDWR | O_CLOEXEC);
    ASSERT_GE(replacement, 0);
    EXPECT_EQ(::flock(replacement, LOCK_EX | LOCK_NB), -1);
    EXPECT_EQ(errno, EWOULDBLOCK);
    ::close(replacement);
    storage.stop();
    EXPECT_EQ(contender.open_storage(directory.c_str(), 4).status, StorageStatus::ok);
}

TEST(NativeHost, missing_directory_is_a_file_error) {
    const TempDirectory directory;
    Host native;
    const ayni::hosts::native::StorageOpen opened =
        native.open_storage(directory.file("missing").c_str(), 4);
    EXPECT_EQ(opened.status, StorageStatus::file_error);
    EXPECT_EQ(opened.error, ENOENT);
}

// A file of another kind, or of a later format version, is left as it is.
TEST(NativeHost, file_that_is_not_this_log_format_is_refused_and_kept) {
    const TempDirectory directory;
    const std::string path = directory.file(Host::log_file_name);
    const Bytes later_version = joined({{'A', 'Y', 'N', 'L'}, le32(2)});
    for (const Bytes &content : {Bytes{'n', 'o', 't', ' ', 'a', ' ', 'l', 'o', 'g', '\n'},
                                 Bytes{'x', 'y'}, later_version}) {
        write_file(path, content);
        Host native;
        EXPECT_EQ(native.open_storage(directory.path().c_str(), 4).status,
                  StorageStatus::not_a_log);
        std::ifstream in(path, std::ios::binary);
        EXPECT_EQ(Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()),
                  content);
    }
}

// The file header is synced before any frame, so a file holding only part
// of it, or only zeros where it would be, holds no batch.
TEST(NativeHost, header_torn_at_creation_is_written_again) {
    const Bytes torn_headers[] = {{'A', 'Y', 'N'}, Bytes(8, 0), Bytes(3, 0)};
    for (const Bytes &torn : torn_headers) {
        const TempDirectory directory;
        write_file(directory.file(Host::log_file_name), torn);
        Host native;
        ASSERT_EQ(native.open_storage(directory.path().c_str(), 4).status, StorageStatus::ok);
        native.close_storage();
        std::ifstream in(directory.file(Host::log_file_name), std::ios::binary);
        EXPECT_EQ(Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()),
                  joined({{'A', 'Y', 'N', 'L'}, le32(1)}));
    }
}

// The layout documented in hosts/native/file_log.h, with CRC-32/ISO-HDLC as
// the kernel computes it (contracts/storage/records.md).
TEST(NativeHost, log_file_frames_each_batch_with_two_crcs) {
    NativeFileStorage storage;
    StorageRequests requests(storage);
    const Bytes payload{0x10, 0x20, 0x30};
    ASSERT_TRUE(requests.append_now(0x0102030405060708U, payload));
    storage.stop();

    const Bytes header =
        joined({{'A', 'Y', 'N', 'F'}, le32(3), {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}});
    const Bytes expected = joined({{'A', 'Y', 'N', 'L'},
                                   le32(1),
                                   header,
                                   le32(ayni::foundation::crc32(header.data(), header.size())),
                                   payload,
                                   le32(ayni::foundation::crc32(payload.data(), payload.size()))});
    EXPECT_EQ(storage.read_file(), expected);
}

// The torn frame stays in the file until the next append, which cuts it off
// so that nothing of it remains after the new frame.
TEST(NativeHost, torn_tail_is_truncated_before_the_next_append) {
    NativeFileStorage storage;
    StorageRequests requests(storage);
    const Bytes first = test_batch(1, 40);
    const Bytes replacement = test_batch(2, 10);
    ASSERT_TRUE(requests.append_now(1, first));
    static_cast<void>(requests.append(2, test_batch(2, 60)));
    ASSERT_TRUE(storage.cut_power_in_last_append(70)); // keeps 70 of its 84 bytes
    const std::size_t torn_size = storage.read_file().size();
    storage.start();
    requests.reset();
    EXPECT_EQ(storage.read_file().size(), torn_size);

    ASSERT_TRUE(requests.append_now(2, replacement));
    EXPECT_EQ(storage.read_file().size(), 8U + (24U + first.size()) + (24U + replacement.size()));
    EXPECT_EQ(requests.read_now(1), joined({first, replacement}));
}

// C-STORE-009: appending after a damaged frame would store a batch the
// kernel can never reach, so nothing is stored.
TEST(NativeHost, append_to_a_damaged_log_fails) {
    NativeFileStorage storage;
    StorageRequests requests(storage);
    for (uint64_t seq = 1; seq <= 3; ++seq) {
        ASSERT_TRUE(requests.append_now(seq, test_batch(seq, 30)));
    }
    storage.stop();
    const std::size_t size = storage.read_file().size();
    ASSERT_TRUE(storage.damage_batch(1, 22));
    storage.start();
    requests.reset();

    StorageRequest &append = requests.append(4, test_batch(4, 30));
    requests.finish();
    EXPECT_EQ(append.result.outcome, AYNI_WRITE_FAILED);
    EXPECT_EQ(storage.read_file().size(), size);
    EXPECT_EQ(requests.read_now(1), test_batch(1, 30));
    EXPECT_EQ(requests.read_now(2), std::nullopt);
}

// C-HOST-018: closing answers every request already accepted before the
// thread stops, so none is left to post into a destroyed kernel; requests
// after it are rejected without being kept (C-HOST-004).
TEST(NativeHost, closing_storage_answers_every_accepted_request) {
    NativeFileStorage storage;
    StorageRequests requests(storage);
    std::vector<StorageRequest *> accepted;
    for (uint64_t seq = 1; seq <= 8; ++seq) {
        accepted.push_back(&requests.append(seq, test_batch(seq, 20)));
    }
    storage.native().close_storage();
    EXPECT_EQ(requests.pool().posted_count(), accepted.size());
    requests.finish();
    for (const StorageRequest *request : accepted) {
        EXPECT_EQ(request->issued, IssueResult::in_flight);
        EXPECT_EQ(request->applied, 1);
        EXPECT_EQ(request->result.outcome, AYNI_WRITE_OK);
    }
    EXPECT_EQ(requests.append(9, test_batch(9, 20)).issued, IssueResult::rejected);
    EXPECT_EQ(requests.read(1, 256).issued, IssueResult::rejected);
}

} // namespace
