#include <ayni/hosts/storage/request.h>
#include <ayni/storage_format.h>
namespace ayni::hosts::storage {
bool append_group_request(const ayni_storage_batch_v1 *batches, uint32_t count,
                          ayni_attempt_ref_v1 attempt, ayni_completion_t *completion,
                          Request &out) {
    if (!batches || !count || count > AYNI_STORAGE_APPEND_COUNT_MAX || !completion) {
        return false;
    }
    Request request{};
    request.port = Request::Port::append_group;
    request.attempt = attempt;
    request.completion = completion;
    request.writer_epoch = batches[0].writer_epoch;
    request.commit_seq = batches[0].commit_seq;
    request.transition_id = batches[0].transition_id;
    request.batch_count = count;
    if (request.commit_seq > UINT64_MAX - count || request.transition_id > UINT64_MAX - count) {
        return false;
    }
    for (uint32_t at = 0; at < count; ++at) {
        const auto &batch = batches[at];
        ayni_storage_batch_identity_v1 id{};
        if (batch.struct_size != sizeof(batch) || !batch.writer_epoch ||
            batch.writer_epoch != request.writer_epoch ||
            batch.commit_seq != request.commit_seq + at ||
            batch.transition_id != request.transition_id + at ||
            !ayni_storage_read_batch_identity(batch.encoded.data, batch.encoded.size, &id) ||
            id.writer_epoch != batch.writer_epoch || id.commit_seq != batch.commit_seq ||
            id.transition_id != batch.transition_id) {
            return false;
        }
        request.batches[at] = batch.encoded;
    }
    out = request;
    return true;
}

ayni_completion_result_v1 serve_request(FileLog &log, const Request &request) {
    FileLog::Result done{AYNI_WRITE_FAILED, 0};
    const bool mutation =
        request.port == Request::Port::append || request.port == Request::Port::append_group ||
        request.port == Request::Port::write_snapshot || request.port == Request::Port::trim;
    if (!mutation || request.writer_epoch >= log.writer_epoch()) {
        if (mutation) {
            log.observe_writer(request.writer_epoch);
        }
        switch (request.port) {
        case Request::Port::append_group:
            done = log.append_many(request.commit_seq, request.batches.data(), request.batch_count);
            break;
        case Request::Port::append:
            done = log.append(request.commit_seq, request.data, request.size);
            break;
        case Request::Port::read_prefix:
            done = log.read_prefix(request.commit_seq, request.buffer, request.size);
            break;
        case Request::Port::read_log:
            done = log.read_log(request.commit_seq, request.buffer, request.size);
            break;
        case Request::Port::fence:
            done = log.fence(request.writer_epoch);
            break;
        case Request::Port::write_snapshot:
            done = log.write_snapshot(request.slot, request.writer_epoch, request.commit_seq,
                                      request.data, request.size);
            break;
        case Request::Port::read_snapshot:
            done =
                log.read_snapshot(request.slot, request.commit_seq, request.buffer, request.size);
            break;
        case Request::Port::trim:
            done = log.trim_prefix(request.writer_epoch, request.commit_seq, request.transition_id);
            break;
        }
    }
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.outcome = done.outcome;
    result.attempt = request.attempt;
    result.value = done.value;
    result.next_cursor = done.cursor;
    return result;
}

} // namespace ayni::hosts::storage
