// C-HVAL-001/002/003/004: outer values use public Host ports, independent of Core records.
// Normal close/reopen is within the explicit independent-consumption authorization.
#include "storage_hosts.h"
#include <array>
#include <atomic>
#include <ayni/hosts/native.h>
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
namespace {
using namespace ayni::hosts::storage;
struct Answer {
    std::atomic<bool> done{false};
    FileValueResult result;
    static void complete(void *context, const FileValueResult &value) noexcept {
        auto &self = *static_cast<Answer *>(context);
        self.result = value;
        self.done.store(true, std::memory_order_release);
    }
};
class FileValues : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(host.open_storage(directory.path().c_str(), 2).status,
                  ayni::hosts::native::StorageStatus::ok);
    }
    FileValueResult invoke(FileValueRequest request) {
        if (next_answer == answers.size()) {
            return {AYNI_CAPACITY_EXCEEDED};
        }
        auto &answer = answers[next_answer++];
        request.context = &answer;
        request.complete = Answer::complete;
        const auto port = host.value_store();
        if (!port.submit || port.submit(port.context, &request) != AYNI_SUBMIT_ACCEPTED) {
            return {AYNI_INVALID_STATE};
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!answer.done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                // Retain each callback context and input through joined close on failure too.
                host.close_storage();
                return {AYNI_BUSY};
            }
            host.wait_for_wake(std::chrono::milliseconds(1));
        }
        return answer.result;
    }
    FileValueRequest write(FileValueKey key, uint64_t previous, uint64_t generation,
                           const std::array<uint8_t, 3> &bytes) {
        FileValueRequest request;
        request.kind = FileValueRequest::Kind::write;
        request.key = key;
        request.previous_generation = previous;
        request.generation = generation;
        request.data = bytes.data();
        request.size = static_cast<uint32_t>(bytes.size());
        return request;
    }
    FileValueRequest read(FileValueKey key, std::array<uint8_t, 3> &out) {
        FileValueRequest request;
        request.key = key;
        request.buffer = out.data();
        request.size = static_cast<uint32_t>(out.size());
        return request;
    }
    ayni::test::TempDirectory directory;
    std::array<Answer, 8> answers{}; // Contexts survive until Host's destructor joins its worker.
    std::size_t next_answer = 0;
    ayni::hosts::native::Host host;
};
TEST_F(FileValues, durable_generation_write_retry_and_normal_reopen_preserve_exact_bytes) {
    const std::array<uint8_t, 3> original{{1, 2, 3}}, updated{{4, 5, 6}};
    ASSERT_EQ(invoke(write({1, 2}, 0, 1, original)).status, AYNI_OK);
    // A retry reconfirms the same version, without consuming a new logical generation.
    EXPECT_EQ(invoke(write({1, 2}, 0, 1, original)).status, AYNI_OK);
    EXPECT_EQ(invoke(write({1, 2}, 0, 1, updated)).status, AYNI_STALE);
    std::array<uint8_t, 3> out{};
    ASSERT_EQ(invoke(read({1, 2}, out)).status, AYNI_OK);
    EXPECT_EQ(out, original);
    ASSERT_EQ(invoke(write({1, 2}, 1, 2, updated)).status, AYNI_OK);
    host.close_storage();
    ASSERT_EQ(host.open_storage(directory.path().c_str(), 2).status,
              ayni::hosts::native::StorageStatus::ok);
    const auto found = invoke(read({1, 2}, out));
    EXPECT_EQ(found.status, AYNI_OK);
    EXPECT_EQ(found.generation, 2U);
    EXPECT_EQ(out, updated);
    host.close_storage();
}
TEST_F(FileValues, owner_object_keys_and_required_read_capacity_remain_explicit) {
    const std::array<uint8_t, 3> first{{1, 2, 3}}, second{{4, 5, 6}};
    ASSERT_EQ(invoke(write({1, 2}, 0, 1, first)).status, AYNI_OK);
    ASSERT_EQ(invoke(write({2, 2}, 0, 1, second)).status, AYNI_OK);
    FileValueRequest query;
    query.key = {1, 2};
    const auto sized = invoke(query);
    EXPECT_EQ(sized.status, AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(sized.size, first.size());
    std::array<uint8_t, 3> out{};
    EXPECT_EQ(invoke(read({1, 3}, out)).status, AYNI_NOT_FOUND);
    EXPECT_EQ(invoke(read({2, 2}, out)).status, AYNI_OK);
    EXPECT_EQ(out, second);
    host.close_storage();
}
TEST_F(FileValues, close_answers_an_accepted_value_and_old_ports_refuse_new_borrows) {
    const std::array<uint8_t, 3> bytes{{1, 2, 3}};
    auto request = write({1, 2}, 0, 1, bytes);
    request.context = &answers[0];
    request.complete = Answer::complete;
    const auto port = host.value_store();
    ASSERT_NE(port.submit, nullptr);
    EXPECT_EQ(port.submit(port.context, &request), AYNI_SUBMIT_ACCEPTED);
    host.close_storage();
    EXPECT_TRUE(answers[0].done.load());
    EXPECT_EQ(answers[0].result.status, AYNI_OK);
    EXPECT_EQ(port.submit(port.context, &request), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    EXPECT_EQ(host.value_store().submit, nullptr);
}
// Bodies go through the same value port, chunked over several files.
class FileBodies : public FileValues {
  protected:
    template <class Done> bool until(Done done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!done()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            host.wait_for_wake(std::chrono::milliseconds(1));
        }
        return true;
    }
    std::size_t files() const {
        std::size_t count = 0;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(directory.path())) {
            count += entry.is_regular_file() ? 1U : 0U;
        }
        return count;
    }
};
TEST_F(FileBodies, released_bodies_leave_no_files) {
    const auto before = files();
    ayni::runtime::FileBodies bodies, reopened;
    ASSERT_EQ(bodies.configure(host.value_store()), AYNI_OK);
    ASSERT_EQ(reopened.configure(host.value_store()), AYNI_OK);
    std::string text(2 * file_value_bytes_max + 100, '\0');
    for (std::size_t at = 0; at < text.size(); ++at) {
        text[at] = static_cast<char>('a' + at % 26);
    }
    const ayni_bytes_v1 body{reinterpret_cast<const uint8_t *>(text.data()),
                             static_cast<uint32_t>(text.size())};
    const auto &store = *bodies.store();
    ayni_body_ref_v1 ref{};
    ayni_status_t status = AYNI_NOT_READY;
    ASSERT_TRUE(until(
        [&] { return (status = store.write(store.context, 7, body, &ref)) != AYNI_NOT_READY; }));
    ASSERT_EQ(status, AYNI_OK);
    EXPECT_EQ(files(), before + 3);
    // A store without the cached copy loads the same body back from its files.
    std::string out(text.size(), '\0');
    const auto &other = *reopened.store();
    ASSERT_TRUE(until([&] {
        return (status = other.read(other.context, &ref, reinterpret_cast<uint8_t *>(out.data()),
                                    static_cast<uint32_t>(out.size()))) != AYNI_NOT_READY;
    }));
    ASSERT_EQ(status, AYNI_OK);
    EXPECT_EQ(out, text);
    store.release(store.context, &ref);
    ASSERT_TRUE(until([&] { return ayni::runtime::FileBodies::ready(&bodies, 0) != 0; }));
    EXPECT_EQ(files(), before);
    host.close_storage();
}
// Shared cache limits affect only resident bytes; durable references keep their files.
TEST_F(FileBodies, trimming_and_a_payload_budget_preserve_referenced_disk_bodies) {
    ayni::runtime::FileBodies bodies;
    ASSERT_EQ(bodies.configure(host.value_store()), AYNI_OK);
    const std::string text = "persistent bytes";
    ASSERT_EQ(bodies.set_cache_limit(text.size()), AYNI_OK);
    const auto *store = bodies.store();
    ayni_body_ref_v1 ref{};
    ayni_status_t status = AYNI_NOT_READY;
    ASSERT_TRUE(until([&] {
        status = store->write(
            store->context, 1,
            {reinterpret_cast<const uint8_t *>(text.data()), static_cast<uint32_t>(text.size())},
            &ref);
        return status != AYNI_NOT_READY;
    }));
    ASSERT_EQ(status, AYNI_OK);
    EXPECT_EQ(bodies.cached_bytes(), text.size());
    const auto file_count = files();
    ASSERT_EQ(bodies.trim_cache(), AYNI_OK);
    EXPECT_EQ(bodies.cached_bytes(), 0U);
    EXPECT_EQ(files(), file_count);
    std::string out(text.size(), '\0');
    ASSERT_TRUE(until([&] {
        status = store->read(store->context, &ref, reinterpret_cast<uint8_t *>(out.data()),
                             static_cast<uint32_t>(out.size()));
        return status != AYNI_NOT_READY;
    }));
    EXPECT_EQ(status, AYNI_OK);
    EXPECT_EQ(out, text);
    std::string oversized(text.size() + 1, 'x');
    ayni_body_ref_v1 refused{};
    EXPECT_EQ(store->write(store->context, 1,
                           {reinterpret_cast<const uint8_t *>(oversized.data()),
                            static_cast<uint32_t>(oversized.size())},
                           &refused),
              AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(files(), file_count);
    host.close_storage();
}
} // namespace
