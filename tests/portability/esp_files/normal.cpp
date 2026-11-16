// White-box port check: production ESP Files + real LittleFS on a RAM NOR medium.
// ADR-0091/C-HOST-009/010/017 normal file operations, not ESP task/flash acceptance.
#include "littlefs_files.h"
#include <array>
#include <ayni/ayni.h>
#include <ayni/hosts/storage/file_log.h>
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/conversation/file_bodies.h>
#include <ayni/storage_format.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>
namespace {
constexpr std::size_t medium_bytes = 1024 * 1024;
constexpr std::size_t erase_bytes = 4096;
esp_partition_t partition{medium_bytes, false};
std::vector<uint8_t> medium(medium_bytes, 0xff);
std::size_t programmed_bytes = 0;
// Normal asynchronous value borrows, serialized as on the Host worker. No I/O failures injected.
class Values {
  public:
    explicit Values(ayni::hosts::storage::Files &files) : files_(files) {}
    ayni::hosts::storage::FileValuePort port() { return {this, submit}; }
    bool advance() {
        if (!request_) {
            return false;
        }
        const auto request = *request_;
        request_.reset();
        const auto result = ayni::hosts::storage::serve_file_value(files_, request);
        request.complete(request.context, result);
        return true;
    }

  private:
    static ayni_submit_result_t submit(void *context,
                                       const ayni::hosts::storage::FileValueRequest *request) {
        auto &self = *static_cast<Values *>(context);
        if (self.request_) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.request_ = *request;
        return AYNI_SUBMIT_ACCEPTED;
    }
    ayni::hosts::storage::Files &files_;
    std::optional<ayni::hosts::storage::FileValueRequest> request_;
};
#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            std::fprintf(stderr, "%d: %s\n", __LINE__, #expression);                               \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)
} // namespace
const esp_partition_t *esp_partition_find_first(int, int, const char *label) {
    return std::strcmp(label, "ayni_test") == 0 ? &partition : nullptr;
}
esp_err_t esp_partition_read(const esp_partition_t *, std::size_t at, void *out, std::size_t size) {
    if (at > medium.size() || size > medium.size() - at) {
        return -1;
    }
    std::memcpy(out, medium.data() + at, size);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *, std::size_t at, const void *data,
                              std::size_t size) {
    if (at > medium.size() || size > medium.size() - at) {
        return -1;
    }
    const auto *in = static_cast<const uint8_t *>(data);
    for (std::size_t index = 0; index < size; ++index) {
        if ((medium[at + index] & in[index]) != in[index]) {
            return -1;
        }
        medium[at + index] &= in[index];
    }
    programmed_bytes += size;
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *, std::size_t at, std::size_t size) {
    if (at % erase_bytes || size % erase_bytes || at > medium.size() || size > medium.size() - at) {
        return -1;
    }
    std::memset(medium.data() + at, 0xff, size);
    return ESP_OK;
}
int main() {
    using ayni::hosts::esp_idf::LittleFiles;
    using ayni::hosts::storage::FileLog;
    using ayni::hosts::storage::Files;
    // Ordinary unsupported-media input must not be formatted, even with blank-init enabled.
    medium[0] = 0;
    const auto unknown = medium;
    {
        LittleFiles files(true);
        CHECK(files.begin("ayni_test") != 0);
    }
    CHECK(medium == unknown);
    std::fill(medium.begin(), medium.end(), 0xff);
    {
        LittleFiles files(false);
        CHECK(files.begin("ayni_test") != 0);
    }
    CHECK(medium[0] == 0xff);
    {
        LittleFiles files(true);
        CHECK(files.begin("ayni_test") == 0);
        {
            LittleFiles other(false);
            CHECK(other.begin("ayni_test") == EWOULDBLOCK);
        }
        int first = Files::invalid_handle, next = Files::invalid_handle;
        CHECK(files.open("first", Files::Mode::read_write_create, first) == 0);
        CHECK(files.open("replacement", Files::Mode::replace, next) == 0);
        const std::array<uint8_t, 3> old{{1, 2, 3}}, fresh{{4, 5, 6}};
        CHECK(files.write(first, 0, old.data(), old.size()) == 0);
        // A second read token observes the first token's buffered current bytes.
        int reader = Files::invalid_handle;
        CHECK(files.open("first", Files::Mode::read_existing_no_follow, reader) == 0);
        std::array<uint8_t, 3> visible{};
        std::size_t visible_bytes = 0;
        CHECK(files.read(reader, 0, visible.data(), visible.size(), visible_bytes) == 0);
        CHECK(visible_bytes == visible.size() && visible == old);
        files.close(reader);
        CHECK(files.write(next, 0, fresh.data(), fresh.size()) == 0);
        CHECK(files.sync_file(next) == 0);
        CHECK(files.rename("replacement", "first") == 0);
        CHECK(files.sync_namespace() == 0);
        std::array<uint8_t, 3> out{};
        std::size_t count = 0;
        CHECK(files.read(next, 0, out.data(), out.size(), count) == 0 && count == out.size());
        CHECK(out == fresh);
        CHECK(files.write(next, fresh.size(), old.data(), old.size()) == 0);
        CHECK(files.sync_file(next) == 0);
        files.close(first);
        files.close(next);
        CHECK(files.open("first", Files::Mode::read_existing, first) == 0);
        CHECK(files.read(first, 0, out.data(), out.size(), count) == 0 && out == fresh);
        uint64_t length = 0;
        CHECK(files.size(first, length) == 0 && length == fresh.size() + old.size());
        CHECK(files.read(first, fresh.size(), out.data(), out.size(), count) == 0 && out == old);
        files.close(first);
        CHECK(files.remove("first") == 0);
        // C-HVAL-004: an ordinary missing delete must not poison subsequent namespace work.
        CHECK(files.remove("first") == ENOENT);
        CHECK(files.sync_namespace() == 0);
        uint64_t available = 0;
        CHECK(files.free_bytes(available) == 0 && available > 0);
    }
    // Shared log framing and normal close/reopen run on the actual ESP file adapter.
    const std::array<uint8_t, 4> payload{{'t', 'e', 's', 't'}};
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "commit_log").status == FileLog::OpenStatus::ok);
        CHECK(log.append(1, payload.data(), payload.size()).outcome == AYNI_WRITE_OK);
    }
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "commit_log").status == FileLog::OpenStatus::ok);
        std::array<uint8_t, 4> out{};
        CHECK(log.read_log(0, out.data(), out.size()).value == out.size());
        CHECK(out == payload);
    }
    // Host framing treats domain records as opaque. This identity-only prefix is
    // sufficient for Host trim matching; it is deliberately not a valid Core batch.
    // Offsets are the fixed identity prefix in contracts/storage/records.md.
    constexpr std::size_t schema_at = 4, writer_at = 8, sequence_at = 16, transition_at = 24;
    std::array<uint8_t, AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES> activation{};
    std::memcpy(activation.data(), "AYJB", 4);
    activation[schema_at] = AYNI_STORAGE_SCHEMA_CURRENT;
    activation[writer_at] = 1;
    activation[sequence_at] = 2;
    activation[transition_at] = 2;
    ayni_storage_batch_identity_v1 identity{};
    CHECK(ayni_storage_read_batch_identity(activation.data(), activation.size(), &identity));
    CHECK(identity.writer_epoch == 1 && identity.commit_seq == 2 && identity.transition_id == 2);
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "commit_log").status == FileLog::OpenStatus::ok);
        CHECK(log.append(2, activation.data(), activation.size()).outcome == AYNI_WRITE_OK);
        CHECK(log.trim_prefix(1, 2, 2).outcome == AYNI_WRITE_OK);
        CHECK(log.append(3, payload.data(), payload.size()).outcome == AYNI_WRITE_OK);
    }
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "commit_log").status == FileLog::OpenStatus::ok);
        std::array<uint8_t, activation.size() + payload.size()> out{};
        CHECK(log.read_log(0, out.data(), out.size()).value == out.size());
        CHECK(std::memcmp(out.data(), activation.data(), activation.size()) == 0);
        CHECK(std::memcmp(out.data() + activation.size(), payload.data(), payload.size()) == 0);
    }
    // The outer store shares the same leased Files and serialized worker as Core I/O.
    using namespace ayni::hosts::storage;
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "commit_log").status == FileLog::OpenStatus::ok);
        FileValueRequest request;
        request.kind = FileValueRequest::Kind::write;
        request.key = {1, 2};
        request.generation = 1;
        request.data = payload.data();
        request.size = payload.size();
        request.complete = [](void *, const FileValueResult &) noexcept {};
        CHECK(serve_file_value(files, request).status == AYNI_OK);
        CHECK(serve_file_value(files, request).status == AYNI_OK);
        CHECK(log.append(4, payload.data(), payload.size()).outcome == AYNI_WRITE_OK);
    }
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "commit_log").status == FileLog::OpenStatus::ok);
        std::array<uint8_t, payload.size()> out{};
        FileValueRequest request;
        request.key = {1, 2};
        request.buffer = out.data();
        request.size = out.size();
        request.complete = [](void *, const FileValueResult &) noexcept {};
        const auto found = serve_file_value(files, request);
        CHECK(found.status == AYNI_OK && found.generation == 1 && out == payload);
        std::array<uint8_t, payload.size()> journal{};
        CHECK(log.read_prefix(4, journal.data(), journal.size()).value == journal.size());
        CHECK(journal == payload);
        // C-HVAL-004: outer removal is idempotent, including the normal directory confirmation.
        request = {};
        request.key = {1, 2};
        request.kind = FileValueRequest::Kind::remove;
        request.complete = [](void *, const FileValueResult &) noexcept {};
        CHECK(serve_file_value(files, request).status == AYNI_OK);
        CHECK(serve_file_value(files, request).status == AYNI_OK);
        CHECK(log.append(5, payload.data(), payload.size()).outcome == AYNI_WRITE_OK);
    }
    // C-HVAL-004/007, ADR-0104: a discarded staged tail may never have created any files.
    // Cleanup must fully drain and allow a later ordinary write on the same live volume.
    {
        LittleFiles files(false);
        CHECK(files.begin("ayni_test") == 0);
        Values values(files);
        ayni::runtime::FileBodies bodies;
        CHECK(bodies.configure(values.port(), 2U * 1024U * 1024U) == AYNI_OK);
        constexpr uint32_t cache_bytes = 4096, progress_limit = 32;
        constexpr uint64_t scope = 7;
        CHECK(bodies.set_cache_limit(cache_bytes) == AYNI_OK);
        const auto *store = bodies.store();
        ayni_body_ref_v1 ref{};
        CHECK(store->stage(store->context, scope, {payload.data(), payload.size()}, &ref) ==
              AYNI_OK);
        bodies.collect(scope, {});
        for (uint32_t at = 0; at < progress_limit; ++at) {
            const bool idle = ayni::runtime::FileBodies::ready(&bodies, 0) != 0;
            if (!values.advance() && idle) {
                break;
            }
        }
        CHECK(bodies.idle());
        CHECK(ayni::runtime::FileBodies::status(&bodies) == AYNI_OK);
        FileValueRequest request;
        request.key = {1, 2};
        request.kind = FileValueRequest::Kind::write;
        request.generation = 1;
        request.data = payload.data();
        request.size = payload.size();
        request.complete = [](void *, const FileValueResult &) noexcept {};
        CHECK(serve_file_value(files, request).status == AYNI_OK);
    }
    // C-HOST-010/017: an ordinary large-log reopen preserves every inherited byte.
    // Revalidation must write linearly rather than copy the LittleFS tail after each chunk.
    constexpr std::size_t large_bytes = 192 * 1024, maximum_rewrite_factor = 4;
    std::vector<uint8_t> large(large_bytes);
    for (std::size_t at = 0; at < large.size(); ++at) {
        large[at] = static_cast<uint8_t>(at);
    }
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "large_log").status == FileLog::OpenStatus::ok);
        CHECK(log.append(1, large.data(), large.size()).outcome == AYNI_WRITE_OK);
    }
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "large_log").status == FileLog::OpenStatus::ok);
        programmed_bytes = 0;
        CHECK(log.fence(2).outcome == AYNI_FENCE_DRAINED);
        std::printf("Large normal reopen programmed %zu bytes for %zu payload bytes\n",
                    programmed_bytes, large.size());
        CHECK(programmed_bytes <= maximum_rewrite_factor * large.size());
        CHECK(log.append(2, payload.data(), payload.size()).outcome == AYNI_WRITE_OK);
    }
    {
        LittleFiles files(false);
        FileLog log(files);
        CHECK(log.open("ayni_test", "large_log").status == FileLog::OpenStatus::ok);
        std::vector<uint8_t> out(large.size() + payload.size());
        CHECK(log.read_log(0, out.data(), out.size()).value == out.size());
        CHECK(std::memcmp(out.data(), large.data(), large.size()) == 0);
        CHECK(std::memcmp(out.data() + large.size(), payload.data(), payload.size()) == 0);
    }
    std::puts("ESP Files normal emulated medium passed; not ESP hardware evidence");
    return 0;
}
