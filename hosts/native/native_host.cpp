#include <ayni/storage_format.h>

#include <ayni/hosts/native.h>

#include "file_log.h"
#include <ayni/hosts/storage/request.h>

#include <cstdint>
#include <cstring>
#include <new>
#include <utility>

#include <pthread.h>
#include <sys/timex.h>
#include <time.h>

namespace ayni::hosts::native {
namespace {

// ADR-0039: idle storage must discover reclaimed space without a new write.
// This is a sampling interval, not a bound on filesystem or scheduler latency.
constexpr auto space_refresh_interval = std::chrono::seconds{1};

uint64_t monotonic_now_us(void * /*ctx*/) {
    const auto since_origin = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(since_origin).count());
}

// Linux clock discipline supplies trust; a merely plausible wall-clock date
// cannot authorize calendar triggers. Both calls are local nonblocking reads.
void system_utc_now(ayni_utc_sample_v1 *out) {
    *out = {sizeof(*out), AYNI_UTC_UNAVAILABLE, 0};
    struct timespec wall {};
    if (::clock_gettime(CLOCK_REALTIME, &wall) != 0) {
        return;
    }
    struct timex discipline {};
    const auto state = ::adjtimex(&discipline);
    out->utc_s = wall.tv_sec;
    out->trust = state >= 0 && state != TIME_ERROR && (discipline.status & STA_UNSYNC) == 0
                     ? AYNI_UTC_TRUSTED
                     : AYNI_UTC_UNTRUSTED;
}

using storage::Request;
using storage::work_reservation;
using storage::WorkItem;

} // namespace

// The request queue and the I/O thread that serves it. The ports and stop()
// run on the owner thread; the log is the I/O thread's alone from the
// moment it starts until stop() joins it.
class Host::Storage {
  public:
    Storage(Host &owner, SpaceQuery query) : owner_(owner), space_query_(query) {}
    ~Storage() { stop(); }
    Storage(const Storage &) = delete;
    Storage &operator=(const Storage &) = delete;

    StorageOpen open(const char *directory, uint32_t capacity);
    ayni_submit_result_t submit(const Request &request) { return enqueue(WorkItem{request}); }
    ayni_submit_result_t submit_value(const storage::FileValueRequest &request) {
        return storage::valid_file_value_request(request) ? enqueue(WorkItem{request})
                                                          : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    // Lets the thread finish every queued request, joins it and closes the
    // log; later submits are rejected.
    void stop();
    ayni_status_t free_bytes(uint64_t *out) {
        const std::lock_guard<std::mutex> lock(mutex_);
        *out = free_bytes_valid_ && free_bytes_ >= reserved_bytes_ ? free_bytes_ - reserved_bytes_
                                                                   : 0U;
        return free_bytes_valid_ ? AYNI_OK : AYNI_STORAGE_FAILED;
    }

  private:
    ayni_submit_result_t enqueue(const WorkItem &request);
    static void *run(void *context);
    void serve_queue();
    bool read_space(uint64_t &available) const;
    // Release only the completed request's reservation and publish one sample.
    // Returns whether the observable cache changed; no Host call under mutex_.
    bool refresh_space(uint64_t completed_reservation);

    Host &owner_;            // Outlives this worker, including its final wake and join.
    SpaceQuery space_query_; // Immutable borrowed file-space probe; default reads FileLog.
    FileLog log_;
    std::unique_ptr<WorkItem[]> queue_; // a ring of capacity_ requests
    uint32_t capacity_ = 0;
    uint32_t head_ = 0;
    uint32_t count_ = 0;
    bool stopping_ = false;
    bool running_ = false; // started and not yet joined; owner thread only
    std::mutex mutex_;
    std::condition_variable signal_;
    pthread_t thread_{};
    // The queue lock publishes one consistent availability/reservation view.
    uint64_t free_bytes_ = 0;
    uint64_t reserved_bytes_ = 0;
    bool free_bytes_valid_ = false;
};

StorageOpen Host::Storage::open(const char *directory, uint32_t capacity) {
    queue_.reset(new (std::nothrow) WorkItem[capacity]);
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
    free_bytes_valid_ = read_space(free_bytes_);
    // pthread rather than std::thread, whose failure is an exception.
    const int error = ::pthread_create(&thread_, nullptr, &Storage::run, this);
    if (error != 0) {
        log_.close();
        return {StorageStatus::thread_error, error};
    }
    running_ = true;
    return {StorageStatus::ok, 0};
}

ayni_submit_result_t Host::Storage::enqueue(const WorkItem &request) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || count_ == capacity_) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        queue_[(head_ + count_) % capacity_] = request;
        ++count_;
        reserved_bytes_ += work_reservation(request);
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
        WorkItem request{};
        bool has_request = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            signal_.wait_for(lock, space_refresh_interval,
                             [this] { return count_ > 0U || stopping_; });
            if (count_ == 0U && stopping_) {
                return; // stopping, and every accepted request is answered
            }
            has_request = count_ != 0U;
            if (has_request) {
                uint32_t selected = 0;
                for (uint32_t at = 1; at < count_; ++at) {
                    if (storage::work_priority(queue_[(head_ + at) % capacity_]) >
                        storage::work_priority(queue_[(head_ + selected) % capacity_])) {
                        selected = at;
                    }
                }
                request = queue_[(head_ + selected) % capacity_];
                // Stable removal preserves ordering within each priority and every Core prefix.
                for (uint32_t at = selected; at + 1U < count_; ++at) {
                    queue_[(head_ + at) % capacity_] = queue_[(head_ + at + 1U) % capacity_];
                }
                --count_;
            }
        }
        if (!has_request) {
            // No completion slot belongs to an idle refresh. An ordinary wake
            // lets Core reconsider admission even after all requests drained.
            if (refresh_space(0)) {
                Host::wake(&owner_);
            }
            continue;
        }
        const auto result = log_.serve(request);
        static_cast<void>(refresh_space(work_reservation(request)));
        // A refused post means the slot was not waiting for this attempt; the
        // kernel counts that, and the Host has nothing else to tell it.
        if (result.post()) {
            Host::wake(&owner_);
        }
    }
}

bool Host::Storage::read_space(uint64_t &available) const {
    return space_query_.read != nullptr ? space_query_.read(space_query_.context, available)
                                        : log_.free_bytes(available);
}

bool Host::Storage::refresh_space(uint64_t completed_reservation) {
    uint64_t available = 0;
    const bool valid = read_space(available);
    // File queries may block. Producers and cached queries retain access to
    // the queue while sampling, including reservations accepted in the meantime.
    const std::lock_guard<std::mutex> lock(mutex_);
    const bool changed = valid != free_bytes_valid_ || (valid && available != free_bytes_);
    free_bytes_ = available;
    free_bytes_valid_ = valid;
    reserved_bytes_ -= completed_reservation;
    return changed;
}

Host::Host() = default;
Host::Host(UtcSource utc) : utc_(utc) {}
void Host::utc_now(void *context, ayni_utc_sample_v1 *out) {
    const auto &source = static_cast<Host *>(context)->utc_;
    if (source.sample) {
        source.sample(source.context, out);
    } else {
        system_utc_now(out);
    }
}

Host::~Host() = default;

ayni_host_v1 Host::table() {
    ayni_host_v1 host{};
    host.struct_size = sizeof(ayni_host_v1);
    host.ctx = this;
    host.monotonic_now_us = &monotonic_now_us;
    host.utc_now = &Host::utc_now;
    host.wake = &Host::wake;
    host.ingress_lock = &Host::ingress_lock;
    host.ingress_unlock = &Host::ingress_unlock;
    if (storage_ != nullptr) {
        host.storage_append = &Host::storage_append;
        host.storage_append_many = &Host::storage_append_many;
        host.storage_read_prefix = &Host::storage_read_prefix;
        host.storage_read_log = &Host::storage_read_log;
        host.storage_fence_writers = &Host::storage_fence_writers;
        host.storage_write_snapshot = &Host::storage_write_snapshot;
        host.storage_read_snapshot = &Host::storage_read_snapshot;
        host.storage_trim_prefix = &Host::storage_trim_prefix;
        host.storage_free_bytes = &Host::storage_free_bytes;
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
    return open_storage(directory, request_capacity, {});
}

StorageOpen Host::open_storage(const char *directory, uint32_t request_capacity, SpaceQuery query) {
    if (directory == nullptr || request_capacity == 0U) {
        return {StorageStatus::invalid_argument, 0};
    }
    if (storage_ != nullptr) {
        return {StorageStatus::already_open, 0};
    }
    std::unique_ptr<Storage> storage(new (std::nothrow) Storage(*this, query));
    if (storage == nullptr) {
        return {StorageStatus::no_memory, 0};
    }
    const StorageOpen opened = storage->open(directory, request_capacity);
    if (opened.status == StorageStatus::ok) {
        storage_ = std::move(storage);
    }
    return opened;
}

storage::FileValuePort Host::value_store() {
    return storage_ ? storage::FileValuePort{this, &Host::submit_value} : storage::FileValuePort{};
}
ayni_submit_result_t Host::submit_value(void *context, const storage::FileValueRequest *request) {
    auto *storage = static_cast<Host *>(context)->storage_.get();
    return storage && request ? storage->submit_value(*request) : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
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
ayni_submit_result_t Host::storage_append_many(void *ctx, const ayni_storage_batch_v1 *batches,
                                               uint32_t count, ayni_attempt_ref_v1 attempt,
                                               ayni_completion_t *completion) {
    auto &self = *static_cast<Host *>(ctx);
    storage::Request request{};
    if (!storage::append_group_request(batches, count, attempt, completion, request)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return self.storage_ ? self.storage_->submit(request) : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_submit_result_t Host::storage_append(void *ctx, const ayni_storage_batch_v1 *batch,
                                          ayni_attempt_ref_v1 attempt,
                                          ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (storage == nullptr || batch == nullptr || completion == nullptr ||
        batch->encoded.data == nullptr || batch->encoded.size == 0U || batch->writer_epoch == 0U) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni_storage_batch_identity_v1 identity{};
    if (ayni_storage_read_batch_identity(batch->encoded.data, batch->encoded.size, &identity) &&
        (identity.writer_epoch != batch->writer_epoch || identity.commit_seq != batch->commit_seq ||
         identity.transition_id != batch->transition_id)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit({Request::Port::append, attempt, completion, batch->commit_seq,
                            batch->encoded.data, nullptr, batch->encoded.size, batch->writer_epoch,
                            batch->transition_id});
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

ayni_submit_result_t Host::storage_read_log(void *ctx, uint64_t cursor, uint8_t *buffer,
                                            uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                            ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (storage == nullptr || completion == nullptr || (buffer == nullptr && buffer_size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit(
        {Request::Port::read_log, attempt, completion, cursor, nullptr, buffer, buffer_size});
}

ayni_submit_result_t Host::storage_fence_writers(void *ctx, uint32_t minimum_epoch,
                                                 ayni_attempt_ref_v1 attempt,
                                                 ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (storage == nullptr || completion == nullptr || minimum_epoch == 0U) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit(
        {Request::Port::fence, attempt, completion, 0, nullptr, nullptr, 0, minimum_epoch});
}

ayni_submit_result_t Host::storage_write_snapshot(void *ctx, uint32_t writer_epoch, uint32_t slot,
                                                  uint64_t offset, ayni_bytes_v1 bytes,
                                                  ayni_attempt_ref_v1 attempt,
                                                  ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (storage == nullptr || completion == nullptr || writer_epoch == 0U || slot > 1U ||
        (bytes.data == nullptr && bytes.size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit({Request::Port::write_snapshot, attempt, completion, offset, bytes.data,
                            nullptr, bytes.size, writer_epoch, 0, slot});
}

ayni_submit_result_t Host::storage_read_snapshot(void *ctx, uint32_t slot, uint64_t offset,
                                                 uint8_t *buffer, uint32_t buffer_size,
                                                 ayni_attempt_ref_v1 attempt,
                                                 ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (storage == nullptr || completion == nullptr || slot > 1U ||
        (buffer == nullptr && buffer_size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit({Request::Port::read_snapshot, attempt, completion, offset, nullptr,
                            buffer, buffer_size, 0, 0, slot});
}

ayni_submit_result_t Host::storage_trim_prefix(void *ctx, uint32_t writer_epoch,
                                               uint64_t transition_id, uint64_t commit_seq,
                                               ayni_attempt_ref_v1 attempt,
                                               ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (storage == nullptr || completion == nullptr || writer_epoch == 0U) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit({Request::Port::trim, attempt, completion, commit_seq, nullptr, nullptr,
                            0, writer_epoch, transition_id});
}

ayni_status_t Host::storage_free_bytes(void *ctx, uint64_t *out_free_bytes) {
    Storage *storage = static_cast<Host *>(ctx)->storage_.get();
    if (out_free_bytes == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (storage == nullptr) {
        return AYNI_STORAGE_FAILED;
    }
    return storage->free_bytes(out_free_bytes);
}

} // namespace ayni::hosts::native
