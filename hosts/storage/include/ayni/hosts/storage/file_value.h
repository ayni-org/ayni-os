// Bounded outer values (e.g. conversation records), separate from Core journal/schema.
#ifndef AYNI_HOSTS_FILE_VALUE_H
#define AYNI_HOSTS_FILE_VALUE_H
#include <ayni/ayni.h>
#include <ayni/hosts/storage/files.h>
namespace ayni::hosts::storage {
inline constexpr uint32_t file_value_bytes_max = 65536;
inline constexpr uint32_t file_value_header_bytes = 48;
// Exact outer owner/object scope. Both identities must be positive; filenames are generated
// internally.
struct FileValueKey {
    uint64_t owner_instance_id = 0;
    uint64_t object_id = 0;
};
// Completion metadata is copied. Read buffer is usable only on OK. Error is errno or zero;
// BUFFER_TOO_SMALL publishes required bytes; RESULT_UNKNOWN never permits a Core context ACK.
struct FileValueResult {
    ayni_status_t status = AYNI_STORAGE_FAILED;
    int error = 0;
    uint64_t generation = 0;
    uint32_t size = 0;
};
// Owner-thread submission. Metadata is copied; buffers stay immutable/alive until completion.
// Callback runs once on the storage worker, must not block/reenter Host or owner-only Core APIs; it
// cannot throw. Completion context lives through Host close/join. No cancellation; close drains
// accepted work.
struct FileValueRequest {
    enum class Kind { read, write } kind = Kind::read;
    FileValueKey key;
    uint64_t previous_generation = 0; // Write CAS: 0 means no file; unused/zero for read.
    uint64_t generation = 0; // Write positive and greater than previous; unused/zero for read.
    const uint8_t *data = nullptr; // Write input only, null permitted for zero bytes.
    uint8_t *buffer = nullptr;     // Read output only, null permitted for zero capacity.
    uint32_t size = 0;             // Payload bytes or read capacity, at most file_value_bytes_max.
    void *context = nullptr;
    void (*complete)(void *, const FileValueResult &) noexcept = nullptr;
};
// Copied asynchronous port. Uses the same bounded Host queue and pending-space accounting
// as Core requests. Unavailable/closed/full/invalid submissions are rejected without retention.
struct FileValuePort {
    void *context = nullptr;
    ayni_submit_result_t (*submit)(void *, const FileValueRequest *) = nullptr;
};
bool valid_file_value_request(const FileValueRequest &request);
// Single file worker only, owns no buffers after return. The Host delivers the completion
// after refreshing its capacity cache. Identical write retry reconfirms bytes durably.
FileValueResult serve_file_value(Files &files, const FileValueRequest &request);
} // namespace ayni::hosts::storage
#endif
