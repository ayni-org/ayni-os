// Shared accepted storage request and execution; scheduling/queues belong to each Host.
#ifndef AYNI_HOSTS_STORAGE_REQUEST_H
#define AYNI_HOSTS_STORAGE_REQUEST_H
#include <ayni/ayni.h>
#include <ayni/hosts/storage/file_log.h>
namespace ayni::hosts::storage {
// One accepted storage call, as the I/O thread needs it once the port has
// returned. data and buffer are the kernel's until the post.
struct Request {
    enum class Port { append, read_prefix, read_log, fence, write_snapshot, read_snapshot, trim };
    Port port;
    ayni_attempt_ref_v1 attempt;
    ayni_completion_t *completion;
    uint64_t commit_seq; // append: the batch's; read: from_commit_seq
    const uint8_t *data; // append: the encoded batch
    uint8_t *buffer;     // read: the kernel's buffer
    uint32_t size;       // encoded size, or buffer size
    uint32_t writer_epoch = 0;
    uint64_t transition_id = 0;
    uint32_t slot = 0;
};

inline uint64_t request_reservation(const Request &request) {
    return request.port == Request::Port::append           ? FileLog::append_bytes(request.size)
           : request.port == Request::Port::write_snapshot ? request.size
                                                           : 0U;
}

// One worker only. Checks installed writer epoch and returns the original attempt identity.
ayni_completion_result_v1 serve_request(FileLog &log, const Request &request);
} // namespace ayni::hosts::storage
#endif
