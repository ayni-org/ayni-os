#include "file_codec.h"
#include "file_io.h"
#include "file_names.h"
#include <algorithm>
#include <array>
#include <ayni/hosts/storage/file_value.h>
#include <cinttypes>
#include <cstdio>
#include <cstring>
namespace ayni::hosts::storage {
namespace {
// AYFV v1 layout is owned by contracts/host/file_values.md, independent of Core records.
constexpr uint32_t format_version = 1;
constexpr std::size_t format_at = 4, owner_at = 8, object_at = 16, generation_at = 24;
constexpr std::size_t size_at = 32, payload_crc_at = 36, header_crc_at = 40, reserved_at = 44;
constexpr std::size_t transfer_bytes = 256, name_bytes = 64;
constexpr char magic[] = "AYFV";
using Header = std::array<uint8_t, file_value_header_bytes>;
struct Contents {
    uint64_t generation = 0;
    uint32_t size = 0;
    uint32_t crc = 0;
};
class File {
  public:
    File(Files &files) : files_(files) {}
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    ~File() { close(); }
    int open(const char *name, Files::Mode mode) { return files_.open(name, mode, handle); }
    void close() {
        if (handle != Files::invalid_handle) {
            files_.close(handle);
            handle = Files::invalid_handle;
        }
    }
    int handle = Files::invalid_handle;

  private:
    Files &files_;
};
FileValueResult failed(int error) {
    return {AYNI_STORAGE_FAILED, error, 0, 0};
}
std::array<char, name_bytes> name_of(FileValueKey key) {
    std::array<char, name_bytes> name{};
    std::snprintf(name.data(), name.size(), "%s%016" PRIx64 "-%016" PRIx64, file_value_prefix,
                  key.owner_instance_id, key.object_id);
    return name;
}
// One reader/writer for the outer Host value header; it does not encode Core records.
Header encode(FileValueKey key, const Contents &contents) {
    Header header{};
    std::memcpy(header.data(), magic, sizeof(magic) - 1);
    codec::store_le32(header.data() + format_at, format_version);
    codec::store_le64(header.data() + owner_at, key.owner_instance_id);
    codec::store_le64(header.data() + object_at, key.object_id);
    codec::store_le64(header.data() + generation_at, contents.generation);
    codec::store_le32(header.data() + size_at, contents.size);
    codec::store_le32(header.data() + payload_crc_at, contents.crc);
    codec::store_le32(header.data() + header_crc_at, codec::crc32(header.data(), header_crc_at));
    return header;
}
FileValueResult inspect(Files &files, int handle, FileValueKey key, Contents &contents) {
    Header header{};
    const int error = io::read_exact(files, handle, 0, header.data(), header.size());
    if (error != 0) {
        return failed(error);
    }
    if (std::memcmp(header.data(), magic, sizeof(magic) - 1) != 0 ||
        codec::load_le32(header.data() + format_at) != format_version) {
        return {AYNI_UNSUPPORTED_VERSION, 0, 0, 0};
    }
    if (codec::load_le32(header.data() + header_crc_at) !=
            codec::crc32(header.data(), header_crc_at) ||
        codec::load_le32(header.data() + reserved_at) != 0 ||
        codec::load_le64(header.data() + owner_at) != key.owner_instance_id ||
        codec::load_le64(header.data() + object_at) != key.object_id) {
        return failed(EIO);
    }
    contents = {codec::load_le64(header.data() + generation_at),
                codec::load_le32(header.data() + size_at),
                codec::load_le32(header.data() + payload_crc_at)};
    if (contents.generation == 0 || contents.size > file_value_bytes_max) {
        return failed(EIO);
    }
    uint64_t bytes = 0;
    const int sized = files.size(handle, bytes);
    if (sized != 0) {
        return failed(sized);
    }
    if (bytes != file_value_header_bytes + static_cast<uint64_t>(contents.size)) {
        return failed(EIO);
    }
    return {AYNI_OK, 0, contents.generation, contents.size};
}
// One payload walk verifies CRC, optionally copies output or compares an idempotent retry.
FileValueResult payload(Files &files, int handle, const Contents &contents, uint8_t *out = nullptr,
                        const uint8_t *expected = nullptr) {
    std::array<uint8_t, transfer_bytes> chunk{};
    uint32_t crc = 0;
    bool same = true;
    for (uint32_t offset = 0; offset < contents.size;) {
        const uint32_t count =
            std::min(contents.size - offset, static_cast<uint32_t>(chunk.size()));
        const int error =
            io::read_exact(files, handle, file_value_header_bytes + offset, chunk.data(), count);
        if (error != 0) {
            return failed(error);
        }
        crc = codec::crc32(chunk.data(), count, crc);
        if (out) {
            std::memcpy(out + offset, chunk.data(), count);
        }
        if (expected && std::memcmp(expected + offset, chunk.data(), count) != 0) {
            same = false;
        }
        offset += count;
    }
    if (crc != contents.crc) {
        return failed(EIO);
    }
    return {same ? AYNI_OK : AYNI_STALE, 0, contents.generation, contents.size};
}
FileValueResult read(Files &files, const FileValueRequest &request, const char *name) {
    File file(files);
    const int opened = file.open(name, Files::Mode::read_existing_no_follow);
    if (opened != 0) {
        return opened == ENOENT ? FileValueResult{AYNI_NOT_FOUND, 0, 0, 0} : failed(opened);
    }
    Contents contents;
    const auto inspected = inspect(files, file.handle, request.key, contents);
    if (inspected.status != AYNI_OK) {
        return inspected;
    }
    if (contents.size > request.size) {
        return {AYNI_BUFFER_TOO_SMALL, 0, contents.generation, contents.size};
    }
    return payload(files, file.handle, contents, request.buffer);
}
FileValueResult check_previous(Files &files, const FileValueRequest &request, const char *name) {
    File file(files);
    const int opened = file.open(name, Files::Mode::read_existing_no_follow);
    if (opened == ENOENT) {
        return {request.previous_generation == 0 ? AYNI_OK : AYNI_STALE, 0, 0, 0};
    }
    if (opened != 0) {
        return failed(opened);
    }
    Contents contents;
    auto result = inspect(files, file.handle, request.key, contents);
    if (result.status != AYNI_OK) {
        return result;
    }
    if (contents.generation == request.previous_generation) {
        return payload(files, file.handle, contents);
    }
    if (contents.generation != request.generation || contents.size != request.size) {
        return {AYNI_STALE, 0, contents.generation, contents.size};
    }
    return payload(files, file.handle, contents, nullptr, request.data);
}
// Atomic replacement is separate from durable confirmation. Identical retries write/sync
// a fresh candidate rather than treating inherited readable bytes as a durability proof.
FileValueResult write(Files &files, const FileValueRequest &request, const char *name) {
    const auto previous = check_previous(files, request, name);
    if (previous.status != AYNI_OK) {
        return previous;
    }
    std::array<char, name_bytes> candidate{};
    constexpr char candidate_suffix[] = ".candidate";
    const auto name_size = std::strlen(name);
    if (name_size > candidate.size() - sizeof(candidate_suffix)) {
        return failed(ENAMETOOLONG);
    }
    std::memcpy(candidate.data(), name, name_size);
    std::memcpy(candidate.data() + name_size, candidate_suffix, sizeof(candidate_suffix));
    File next(files);
    int error = next.open(candidate.data(), Files::Mode::replace);
    if (error != 0) {
        return failed(error);
    }
    const Contents contents{request.generation, request.size,
                            codec::crc32(request.data, request.size)};
    const auto header = encode(request.key, contents);
    error = files.write(next.handle, 0, header.data(), header.size());
    if (error == 0 && request.size) {
        error = files.write(next.handle, header.size(), request.data, request.size);
    }
    if (error == 0) {
        error = files.sync_file(next.handle);
    }
    next.close();
    if (error != 0) {
        return failed(error);
    }
    error = files.rename(candidate.data(), name);
    if (error == 0) {
        error = files.sync_namespace();
    }
    if (error != 0) {
        return {AYNI_RESULT_UNKNOWN, error, 0, 0};
    }
    return {AYNI_OK, 0, contents.generation, contents.size};
}
} // namespace
bool valid_file_value_request(const FileValueRequest &request) {
    if (!request.complete || !request.key.owner_instance_id || !request.key.object_id ||
        request.size > file_value_bytes_max) {
        return false;
    }
    switch (request.kind) {
    case FileValueRequest::Kind::read:
        return !request.previous_generation && !request.generation && !request.data &&
               (request.buffer || request.size == 0);
    case FileValueRequest::Kind::write:
        return request.generation > request.previous_generation && !request.buffer &&
               (request.data || request.size == 0);
    }
    return false;
}
FileValueResult serve_file_value(Files &files, const FileValueRequest &request) {
    if (!valid_file_value_request(request)) {
        return {AYNI_INVALID_ARGUMENT, 0, 0, 0};
    }
    const auto name = name_of(request.key);
    return request.kind == FileValueRequest::Kind::read ? read(files, request, name.data())
                                                        : write(files, request, name.data());
}
} // namespace ayni::hosts::storage
