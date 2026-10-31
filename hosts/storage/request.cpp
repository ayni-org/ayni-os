#include <ayni/hosts/storage/request.h>
namespace ayni::hosts::storage {
ayni_completion_result_v1 serve_request(FileLog &log, const Request &request) {
    FileLog::Result done{AYNI_WRITE_FAILED, 0};
    const bool mutation = request.port == Request::Port::append ||
                          request.port == Request::Port::write_snapshot ||
                          request.port == Request::Port::trim;
    if (!mutation || request.writer_epoch >= log.writer_epoch()) {
        if (mutation) {
            log.observe_writer(request.writer_epoch);
        }
        switch (request.port) {
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
