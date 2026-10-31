// Shared file mechanism (C-HOST-009/010/017): real Native files preserve framing,
// normal reopen and exclusive namespace ownership through rejected consumers.
#include "../../hosts/native/posix_files.h"
#include "storage_hosts.h"
#include <ayni/hosts/storage/file_log.h>
#include <filesystem>
#include <gtest/gtest.h>
namespace {
using ayni::hosts::native::PosixFiles;
using ayni::hosts::storage::FileLog;
using ayni::test::TempDirectory;

TEST(SharedStorage, reservations_match_actual_append_bytes_and_normal_reopen_keeps_payload) {
    const TempDirectory directory;
    const auto payload = ayni::test::test_batch(1, 128);
    PosixFiles files;
    FileLog log(files);
    ASSERT_EQ(log.open(directory.path().c_str(), "commit_log").status, FileLog::OpenStatus::ok);
    const auto before = std::filesystem::file_size(directory.file("commit_log"));
    ASSERT_EQ(log.append(1, payload.data(), static_cast<uint32_t>(payload.size())).outcome,
              AYNI_WRITE_OK);
    const auto after = std::filesystem::file_size(directory.file("commit_log"));
    EXPECT_EQ(after - before, FileLog::append_bytes(static_cast<uint32_t>(payload.size())));
    log.close();
    ASSERT_EQ(log.open(directory.path().c_str(), "commit_log").status, FileLog::OpenStatus::ok);
    std::vector<uint8_t> found(payload.size());
    const auto read = log.read_log(0, found.data(), static_cast<uint32_t>(found.size()));
    EXPECT_EQ(read.outcome, AYNI_WRITE_OK);
    EXPECT_EQ(read.value, found.size());
    EXPECT_EQ(found, payload);
    // C-HOST-010: normal reopen reconfirms the prefix before a new writer appends.
    ASSERT_EQ(log.fence(2).outcome, AYNI_FENCE_DRAINED);
    const auto next = ayni::test::test_batch(2, 128);
    ASSERT_EQ(log.append(2, next.data(), static_cast<uint32_t>(next.size())).outcome,
              AYNI_WRITE_OK);
    found.resize(payload.size() + next.size());
    EXPECT_EQ(log.read_log(0, found.data(), static_cast<uint32_t>(found.size())).value,
              found.size());
    EXPECT_EQ(found, ayni::test::joined({payload, next}));
}

TEST(SharedStorage, refused_second_engine_does_not_release_the_first_engine_namespace) {
    const TempDirectory directory;
    PosixFiles files;
    FileLog first(files);
    ASSERT_EQ(first.open(directory.path().c_str(), "commit_log").status, FileLog::OpenStatus::ok);
    {
        FileLog second(files);
        EXPECT_NE(second.open(directory.path().c_str(), "commit_log").status,
                  FileLog::OpenStatus::ok);
    }
    // The rejected consumer never acquired this namespace and cannot end its lease.
    const auto payload = ayni::test::test_batch(1, 128);
    EXPECT_EQ(first.append(1, payload.data(), static_cast<uint32_t>(payload.size())).outcome,
              AYNI_WRITE_OK);
    uint64_t available = 0;
    EXPECT_TRUE(first.free_bytes(available));
}

TEST(SharedStorage, closed_engine_cannot_confirm_a_fence_or_write) {
    const TempDirectory directory;
    PosixFiles files;
    FileLog log(files);
    ASSERT_EQ(log.open(directory.path().c_str(), "commit_log").status, FileLog::OpenStatus::ok);
    log.close();
    const auto payload = ayni::test::test_batch(1, 128);
    EXPECT_EQ(log.append(1, payload.data(), static_cast<uint32_t>(payload.size())).outcome,
              AYNI_WRITE_FAILED);
    EXPECT_EQ(log.fence(1).outcome, AYNI_FENCE_UNAVAILABLE);
    uint64_t available = 17;
    EXPECT_FALSE(log.free_bytes(available));
    EXPECT_EQ(available, 0U);
}

TEST(SharedStorage, non_flat_or_reserved_name_refusal_does_not_consume_the_namespace) {
    const TempDirectory directory;
    PosixFiles files;
    FileLog log(files);
    for (const char *name : {"../outside", "snapshot_0", "snapshot_1", ".ayni_storage_lock",
                             "value-1", "value-1.candidate"}) {
        EXPECT_EQ(log.open(directory.path().c_str(), name).status, FileLog::OpenStatus::file_error);
    }
    EXPECT_EQ(log.open(directory.path().c_str(), "commit_log").status, FileLog::OpenStatus::ok);
    EXPECT_NE(files.rename("commit_log", ".ayni_storage_lock"), 0);
    EXPECT_NE(files.rename(".ayni_storage_lock", "moved"), 0);
    EXPECT_NE(files.remove(".ayni_storage_lock"), 0);
    const auto payload = ayni::test::test_batch(1, 128);
    EXPECT_EQ(log.append(1, payload.data(), static_cast<uint32_t>(payload.size())).outcome,
              AYNI_WRITE_OK);
}
} // namespace
