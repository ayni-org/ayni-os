// White-box port check: production ESP Files + real LittleFS on a RAM NOR medium.
// ADR-0091/C-HOST-009/010/017 normal file operations, not ESP task/flash acceptance.
#include "littlefs_files.h"
#include <array>
#include <ayni/ayni.h>
#include <ayni/hosts/storage/file_log.h>
#include <ayni/storage_format.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
namespace {
constexpr std::size_t medium_bytes = 1024 * 1024;
constexpr std::size_t erase_bytes = 4096;
esp_partition_t partition{medium_bytes, false};
std::vector<uint8_t> medium(medium_bytes, 0xff);
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
        CHECK(files.sync_file(first) == 0);
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
    std::puts("ESP Files normal emulated medium passed; not ESP hardware evidence");
    return 0;
}
