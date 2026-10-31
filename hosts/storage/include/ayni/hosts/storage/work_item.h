// The storage worker's bounded queue item: Core journal work or one outer value operation.
#ifndef AYNI_HOSTS_STORAGE_WORK_ITEM_H
#define AYNI_HOSTS_STORAGE_WORK_ITEM_H
#include <ayni/hosts/storage/file_value.h>
#include <ayni/hosts/storage/request.h>
#include <variant>
namespace ayni::hosts::storage {
// In-use entries hold only submissions validated before admission; empty ring entries are unread.
struct WorkItem {
    std::variant<Request, FileValueRequest> value;
    WorkItem() : value(Request{}) {}
    WorkItem(const Request &request) : value(request) {}
    WorkItem(const FileValueRequest &request) : value(request) {}
};
inline uint64_t work_reservation(const WorkItem &work) {
    if (const auto *request = std::get_if<Request>(&work.value)) {
        return request_reservation(*request);
    }
    const auto &request = *std::get_if<FileValueRequest>(&work.value);
    return request.kind == FileValueRequest::Kind::write
               ? static_cast<uint64_t>(request.size) + file_value_header_bytes
               : 0;
}
struct CoreCompleted {
    ayni_completion_t *completion;
    ayni_completion_result_v1 result;
};
struct ValueCompleted {
    void *context;
    void (*complete)(void *, const FileValueResult &) noexcept;
    FileValueResult result;
};
// Worker retains this copied result until cache reservations are released, then posts once.
struct WorkResult {
    std::variant<CoreCompleted, ValueCompleted> value;
    // True means outer completion needs the Host owner wake (Core posts wake themselves).
    bool post() const {
        if (const auto *core = std::get_if<CoreCompleted>(&value)) {
            static_cast<void>(ayni_completion_post(core->completion, &core->result));
            return false;
        } else {
            const auto &outer = *std::get_if<ValueCompleted>(&value);
            outer.complete(outer.context, outer.result);
            return true;
        }
    }
};
inline WorkResult serve_work(FileLog &log, Files &files, const WorkItem &work) {
    if (const auto *request = std::get_if<Request>(&work.value)) {
        return {CoreCompleted{request->completion, serve_request(log, *request)}};
    }
    const auto &request = *std::get_if<FileValueRequest>(&work.value);
    return {ValueCompleted{request.context, request.complete, serve_file_value(files, request)}};
}
} // namespace ayni::hosts::storage
#endif
