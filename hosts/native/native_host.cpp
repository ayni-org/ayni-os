#include <ayni/hosts/native.h>

#include "file_log.h"

#include <cstdint>
#include <new>
#include <utility>

#include <pthread.h>

namespace ayni::hosts::native {
namespace {

uint64_t monotonic_now_us(void * /*ctx*/) {
    const auto since_origin = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(since_origin).count());
}

// One accepted storage call, as the I/O thread needs it once the port has
// returned. data and buffer are the kernel's until the post.
struct Request {
    enum class Port { append, read_prefix };
    Port port;
    ayni_attempt_ref_v1 attempt;
    ayni_completion_t *completion;
    uint64_t commit_seq; // append: the batch's; read: from_commit_seq
    const uint8_t *data; // append: the encoded batch
    uint8_t *buffer;     // read: the kernel's buffer
    uint32_t size;       // encoded size, or buffer size
};

} // namespace

// The request queue and the I/O thread that serves it. The ports and stop()
// run on the owner thread; the log is the I/O thread's alone from the
// moment it starts until stop() joins it.
class Host::Storage {
  public:
    Storage() = default;
    ~Storage() { stop(); }
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    StorageOpen open(const char *directory, uint32_t capacity);
    ayni_submit_result_t submit(const Request &request);
    // Lets the thread finish every queued request, joins it and closes the
    // log; later submits are rejected.
    void stop();

  private:
    static void *run(void *context);
    void serve_queue();
    ayni_completion_result_v1 serve(const Request &request);

    FileLog log_;
    std::unique_ptr<Request[]> queue_; // a ring of capacity_ requests
    uint32_t capacity_ = 0;
    uint32_t head_ = 0;
    uint32_t count_ = 0;
    bool stopping_ = false;
    bool running_ = false; // started and not yet joined; owner thread only
    std::mutex mutex_;
    std::condition_variable signal_;
    pthread_t thread_{};
};

StorageOpen Host::Storage::open(const char *directory, uint32_t capacity) {
    queue_.reset(new (std::nothrow) Request[capacity]);
    if (queue_ == nullptr) {
        return {StorageStatus::no_memory, 0};
    }
    capacity_ = capacity;
    const FileLog::Opened opened = log_.open(directory, log_file_name);
    switch (opened.status) {
    case FileLog::OpenStatus::ok:
        break;
    case FileLog::OpenStatus::in_use:
        return {StorageStatus::in_use, 0};
    case FileLog::OpenStatus::not_a_log:
        return {StorageStatus::not_a_log, 0};
    case FileLog::OpenStatus::file_error:
        return {StorageStatus::file_error, opened.error};
    }
    // pthread rather than std::thread, whose failure is an exception.
    const int error = ::pthread_create(&thread_, nullptr, &Storage::run, this);
    if (error != 0) {
        log_.close();
        return {StorageStatus::thread_error, error};
    }
    running_ = true;
    return {StorageStatus::ok, 0};
}

ayni_submit_result_t Host::Storage::submit(const Request &request) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || count_ == capacity_) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        queue_[(head_ + count_) % capacity_] = request;
        ++count_;
    }
    signal_.notify_one();
    return AYNI_SUBMIT_ACCEPTED;
}

void Host::Storage::stop() {
    if (running_) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        signal_.notify_one();
        ::pthread_join(thread_, nullptr);
        running_ = false;
    }
    log_.close();
}

void *Host::Storage::run(void *context) {
    static_cast<Storage *>(context)->serve_queue();
    return nullptr;
}

void Host::Storage::serve_queue() {
    for (;;) {
        Request request{};
        {
            std::unique_lock<std::mutex> lock(mutex_);
            signal_.wait(lock, [this] { return count_ > 0U || stopping_; });
            if (count_ == 0U) {
                return; // stopping, and every accepted request is answered
            }
            request = queue_[head_];
            head_ = (head_ + 1U) % capacity_;
            --count_;
        }
        const ayni_completion_result_v1 result = serve(request);
        // A refused post means the slot was not waiting for this attempt; the
        // kernel counts that, and the Host has nothing else to tell it.
        static_cast<void>(ayni_completion_post(request.completion, &result));
    }
}

ayni_completion_result_v1 Host::Storage::serve(const Request &request) {
    const FileLog::Result done =
        request.port == Request::Port::append
            ? log_.append(request.commit_seq, request.data, request.size)
            : log_.read_prefix(request.commit_seq, request.buffer, request.size);
    ayni_completion_result_v1 result{};
    result.struct_size = sizeof(result);
    result.outcome = done.outcome;
    result.attempt = request.attempt;
    result.value = done.value;
    return result;
}

Host::Host() = default;

Host::~Host() = default;

ayni_host_v1 Host::table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = this;
    host.monotonic_now_us = &monotonic_now_us;
    host.wake = &Host::wake;
    host.ingress_lock = &Host::ingress_lock;
    host.ingress_unlock = &Host::ingress_unlock;
    if (storage_ != nullptr) {
        host.storage_append = &Host::storage_append;
        host.storage_read_prefix = &Host::storage_read_prefix;
    }
    return host;
}

bool Host::wait_for_wake(std::chrono::microseconds timeout) {
    std::unique_lock<std::mutex> lock(wake_mutex_);
    const bool woken = wake_signal_.wait_for(lock, timeout, [this] { return wake_pending_; });
    wake_pending_ = false;
    return woken;
}

StorageOpen Host::open_storage(const char *directory, uint32_t request_capacity) {
    if (directory == nullptr || request_capacity == 0U) {
        return {StorageStatus::invalid_argument, 0};
    }
    if (storage_ != nullptr) {
        return {StorageStatus::already_open, 0};
    }
    std::unique_ptr<Storage> storage(new (std::nothrow) Storage());
    if (storage == nullptr) {
        return {StorageStatus::no_memory, 0};
    }
    const StorageOpen opened = storage->open(directory, request_capacity);
    if (opened.status == StorageStatus::ok) {
        storage_ = std::move(storage);
    }
    return opened;
}

void Host::close_storage() {
    storage_.reset();
}

void Host::wake(void *ctx) {
    Host &self = *static_cast<Host *>(ctx);
    {
        const std::lock_guard<std::mutex> lock(self.wake_mutex_);
        self.wake_pending_ = true;
    }
    self.wake_signal_.notify_one();
}

void Host::ingress_lock(void *ctx) {
    static_cast<Host *>(ctx)->ingress_.lock();
}

void Host::ingress_unlock(void *ctx) {
    static_cast<Host *>(ctx)->ingress_.unlock();
}

// Without a whole batch there is no work to keep; an empty one could not be
// told from the end of the log when read back.
ayni_submit_result_t Host::storage_append(void *ctx, const ayni_storage_batch_v1 *batch,
                                          ayni_attempt_ref_v1 attempt,
                                          ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (storage == nullptr || batch == nullptr || completion == nullptr ||
        batch->encoded.data == nullptr || batch->encoded.size == 0U) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit({Request::Port::append, attempt, completion, batch->commit_seq,
                            batch->encoded.data, nullptr, batch->encoded.size});
}

ayni_submit_result_t Host::storage_read_prefix(void *ctx, uint64_t from_commit_seq, uint8_t *buffer,
                                               uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                               ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (storage == nullptr || completion == nullptr || (buffer == nullptr && buffer_size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit({Request::Port::read_prefix, attempt, completion, from_commit_seq,
                            nullptr, buffer, buffer_size});
}

} // namespace ayni::hosts::native
