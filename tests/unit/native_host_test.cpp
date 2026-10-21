// Native Host services (C-HOST-007, ADR-0015): clock, wake and ingress lock;
// and what is particular to its file storage (MA-010): opening, the file
// format, truncating a torn tail, refusing appends to a damaged log, and
// closing. Behavior shared with the sim Host is in
// host_storage_contract_test.cpp.

#include "foundation/crc32.h"
#include "storage_hosts.h"
#include "temp_directory.h"

#include <ayni/hosts/native.h>

#include <gtest/gtest.h>

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

namespace {

using namespace std::chrono_literals;
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

void write_file(const std::string &path, const Bytes &bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

Bytes le32(uint32_t value) {
    return {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U),
            static_cast<uint8_t>(value >> 16U), static_cast<uint8_t>(value >> 24U)};
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
    ASSERT_EQ(native.open_storage(directory.path().c_str(), 4).status, StorageStatus::ok);
    EXPECT_NE(native.table().storage_append, nullptr);
    EXPECT_NE(native.table().storage_read_prefix, nullptr);
    native.close_storage();
    EXPECT_EQ(native.table().storage_append, nullptr);
    EXPECT_EQ(native.table().storage_read_prefix, nullptr);
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
