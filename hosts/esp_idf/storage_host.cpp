#include "littlefs_files.h"
#include <algorithm>
#include <ayni/hosts/esp_idf.h>
#include <ayni/hosts/storage/work_item.h>
#include <ayni/storage_format.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <new>
namespace ayni::hosts::esp_idf {
using storage::Request;
using storage::WorkItem;
namespace {
constexpr TickType_t space_refresh_ticks = pdMS_TO_TICKS(1000);
constexpr TickType_t space_quiet_ticks = std::max<TickType_t>(1, pdMS_TO_TICKS(20));
constexpr UBaseType_t worker_priority = 1;
constexpr char log_file_name[] = "commit_log";
} // namespace
// One bounded internal-RAM queue/worker. Lock protects queue and cached counters only;
// neither flash I/O nor completion delivery runs with that lock held.
class Host::Storage {
  public:
    Storage(Host &owner, bool initialize_blank)
        : owner_(owner), files_(initialize_blank), log_(files_) {
        done_ = xSemaphoreCreateBinaryStatic(&done_storage_);
    }
    ~Storage() {
        stop();
        vSemaphoreDelete(done_);
    }
    StorageOpen open(const char *label, uint32_t capacity);
    ayni_submit_result_t submit(const Request &request) { return enqueue(WorkItem{request}); }
    ayni_submit_result_t submit_value(const storage::FileValueRequest &request) {
        return storage::valid_file_value_request(request) ? enqueue(WorkItem{request})
                                                          : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    void stop();
    ayni_status_t free_bytes(uint64_t *out);
    uint32_t stack_free_bytes() const {
        return worker_ ? uxTaskGetStackHighWaterMark(worker_) : final_stack_free_;
    }
    StorageMetrics metrics() {
        portENTER_CRITICAL(&mutex_);
        const auto result = metrics_;
        portEXIT_CRITICAL(&mutex_);
        return result;
    }

    uint64_t append_durable_us(uint64_t seq) {
        portENTER_CRITICAL(&mutex_);
        uint64_t at_us = 0;
        for (const auto &append : recent_appends_) {
            if (append.first && seq >= append.first && seq <= append.last) {
                at_us = std::max(at_us, append.at_us);
            }
        }
        portEXIT_CRITICAL(&mutex_);
        return at_us;
    }

  private:
    ayni_submit_result_t enqueue(const WorkItem &request);
    static void run(void *ctx);
    void serve_queue();
    void refresh_idle_space();
    bool refresh_space(uint64_t completed_reservation, bool quiet);
    Host &owner_;
    LittleFiles files_;
    SpaceRefresh space_refresh_;
    storage::FileLog log_;
    struct Queued {
        WorkItem work;
        uint64_t submitted_us = 0; // Monotonic enqueue attempt, used only for queue timing.
    };
    void serve(const Queued &queued);
    std::array<Queued, Host::request_capacity_limit> requests_{};
    struct Appended {
        uint64_t first = 0, last = 0, at_us = 0; // Successful consecutive journal group.
    };
    std::array<Appended, Host::request_capacity_limit> recent_appends_{};
    size_t next_append_ = 0;
    StorageMetrics metrics_{}; // Published copies only; guarded by mutex_, never read live files.
    uint32_t capacity_ = 0, head_ = 0, count_ = 0;
    enum class State { closed, running, draining };
    State state_ = State::closed;
    portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
    uint64_t available_ = 0, reserved_ = 0;
    bool valid_space_ = false;
    StaticSemaphore_t done_storage_{};
    SemaphoreHandle_t done_ = nullptr;
    StaticTask_t task_storage_{};
    std::array<StackType_t, Host::worker_stack_bytes / sizeof(StackType_t)> stack_{};
    TaskHandle_t worker_ = nullptr;
    uint32_t final_stack_free_ = 0;
};
StorageOpen Host::Storage::open(const char *label, uint32_t capacity) {
    const auto opened = log_.open(label, log_file_name);
    using OpenStatus = storage::FileLog::OpenStatus;
    if (opened.status != OpenStatus::ok) {
        if (files_.last_mount_error()) {
            return {StorageStatus::not_a_volume, files_.last_mount_error()};
        }
        return {opened.status == OpenStatus::in_use      ? StorageStatus::in_use
                : opened.status == OpenStatus::not_a_log ? StorageStatus::not_a_log
                                                         : StorageStatus::file_error,
                opened.error};
    }
    capacity_ = capacity;
    valid_space_ = log_.free_bytes(available_);
    metrics_.files = files_.metrics();
    state_ = State::running;
    worker_ = xTaskCreateStatic(run, "ayni.storage", Host::worker_stack_bytes, this,
                                worker_priority, stack_.data(), &task_storage_);
    if (!worker_) {
        state_ = State::closed;
        log_.close();
        return {StorageStatus::task_error, 0};
    }
    return {StorageStatus::ok, 0};
}
ayni_submit_result_t Host::Storage::enqueue(const WorkItem &request) {
    bool accepted = false;
    const auto submitted = static_cast<uint64_t>(esp_timer_get_time());
    portENTER_CRITICAL(&mutex_);
    if (state_ == State::running && count_ < capacity_) {
        requests_[(head_ + count_) % capacity_] = {request, submitted};
        ++count_;
        reserved_ += storage::work_reservation(request);
        accepted = true;
    }
    portEXIT_CRITICAL(&mutex_);
    if (accepted) {
        xTaskNotifyGive(worker_);
    }
    return accepted ? AYNI_SUBMIT_ACCEPTED : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
ayni_status_t Host::Storage::free_bytes(uint64_t *out) {
    portENTER_CRITICAL(&mutex_);
    *out = valid_space_ && available_ >= reserved_ ? available_ - reserved_ : 0;
    const auto status = valid_space_ ? AYNI_OK : AYNI_STORAGE_FAILED;
    portEXIT_CRITICAL(&mutex_);
    return status;
}
bool Host::Storage::refresh_space(uint64_t completed_reservation, bool quiet) {
    portENTER_CRITICAL(&mutex_);
    const auto outstanding = reserved_ - completed_reservation;
    portEXIT_CRITICAL(&mutex_);
    const auto need = files_.space_reconciliation(outstanding);
    const bool measure = space_refresh_.measure(need, uint64_t(esp_timer_get_time()), quiet);
    uint64_t available = 0;
    const bool valid = (!measure || files_.measure_space() == 0) && log_.free_bytes(available);
    portENTER_CRITICAL(&mutex_);
    const bool changed = valid != valid_space_ || (valid && available != available_);
    valid_space_ = valid;
    available_ = available;
    reserved_ -= completed_reservation;
    portEXIT_CRITICAL(&mutex_);
    return changed;
}
void Host::Storage::run(void *ctx) {
    auto &self = *static_cast<Storage *>(ctx);
    self.serve_queue();
    self.final_stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
    xSemaphoreGive(self.done_);
    // Never self-delete: the owner waits for suspended state before deleting the
    // task, so no running task or deferred self-delete can access freed static memory.
    for (;;) {
        vTaskSuspend(nullptr);
    }
}
void Host::Storage::refresh_idle_space() {
    const auto started = esp_timer_get_time();
    const bool changed = refresh_space(0, true);
    const auto files = files_.metrics();
    const auto elapsed = static_cast<uint64_t>(esp_timer_get_time() - started);
    portENTER_CRITICAL(&mutex_);
    metrics_.idle_space_us += elapsed;
    ++metrics_.idle_refreshes;
    metrics_.idle_measures += files.space_measure_calls - metrics_.files.space_measure_calls;
    metrics_.idle_measure_us += files.space_measure_us - metrics_.files.space_measure_us;
    metrics_.files = files;
    portEXIT_CRITICAL(&mutex_);
    if (changed) {
        Host::wake(&owner_);
    }
}
void Host::Storage::serve_queue() {
    bool quiet = false;
    for (;;) {
        Queued queued{};
        bool has_request = false, finished = false;
        portENTER_CRITICAL(&mutex_);
        has_request = count_ != 0;
        finished = !has_request && state_ == State::draining;
        if (has_request) {
            uint32_t selected = 0;
            for (uint32_t at = 1; at < count_; ++at) {
                if (storage::work_priority(requests_[(head_ + at) % capacity_].work) >
                    storage::work_priority(requests_[(head_ + selected) % capacity_].work)) {
                    selected = at;
                }
            }
            queued = requests_[(head_ + selected) % capacity_];
            // Stable priority preserves the complete ordered Core prefix.
            for (uint32_t at = selected; at + 1U < count_; ++at) {
                requests_[(head_ + at) % capacity_] = requests_[(head_ + at + 1U) % capacity_];
            }
            --count_;
        }
        portEXIT_CRITICAL(&mutex_);
        if (finished) {
            return;
        }
        if (!has_request) {
            if (quiet) {
                refresh_idle_space();
            }
            // Give a woken owner a chance to enqueue its follow-up. The queue is checked again
            // before scanning, and routine work has a hard deadline even when it never drains.
            const auto timeout = space_refresh_.pending() ? space_quiet_ticks : space_refresh_ticks;
            quiet = ulTaskNotifyTake(pdTRUE, timeout) == 0;
            continue;
        }
        quiet = false;
        serve(queued);
    }
}
// Preserve serve -> refresh reservations -> publish completed counters -> post/wake -> add post_us
// so an owner sampling immediately upon completion observes the I/O that triggered the transition.
void Host::Storage::serve(const Queued &queued) {
    const auto started = static_cast<uint64_t>(esp_timer_get_time());
    const auto result = storage::serve_work(log_, files_, queued.work);
    const auto served = static_cast<uint64_t>(esp_timer_get_time());
    static_cast<void>(refresh_space(storage::work_reservation(queued.work), false));
    const auto refreshed = static_cast<uint64_t>(esp_timer_get_time());
    const auto file_metrics = files_.metrics();
    const auto scan_frames = log_.append_scan_frames();
    const auto queue_us = started - queued.submitted_us;
    const auto service_us = served - started;
    const auto space_us = refreshed - served;
    portENTER_CRITICAL(&mutex_);
    ++metrics_.completed;
    metrics_.queue_us += queue_us;
    metrics_.io_us += service_us;
    metrics_.space_us += space_us;
    metrics_.max_queue_us = std::max(metrics_.max_queue_us, queue_us);
    metrics_.max_io_us = std::max(metrics_.max_io_us, service_us);
    metrics_.max_space_us = std::max(metrics_.max_space_us, space_us);
    if (const auto *core = std::get_if<Request>(&queued.work.value)) {
        if (core->port == Request::Port::append || core->port == Request::Port::append_group) {
            ++metrics_.append_calls;
            metrics_.append_us += service_us;
            if (std::get<storage::CoreCompleted>(result.value).result.outcome == AYNI_WRITE_OK) {
                const auto count = core->port == Request::Port::append ? 1U : core->batch_count;
                recent_appends_[next_append_] = {core->commit_seq, core->commit_seq + count - 1U,
                                                 served};
                next_append_ = (next_append_ + 1U) % recent_appends_.size();
            }
        }
    } else if (const auto *value = std::get_if<storage::FileValueRequest>(&queued.work.value)) {
        metrics_.record_value(*value, std::get<storage::ValueCompleted>(result.value).result,
                              service_us, refreshed - queued.submitted_us);
        metrics_.value_us += service_us;
        switch (value->kind) {
        case storage::FileValueRequest::Kind::read:
            ++metrics_.value_read_calls;
            metrics_.value_read_us += service_us;
            break;
        case storage::FileValueRequest::Kind::write:
            ++metrics_.value_write_calls;
            metrics_.value_write_us += service_us;
            break;
        case storage::FileValueRequest::Kind::remove:
            ++metrics_.value_remove_calls;
            metrics_.value_remove_us += service_us;
            break;
        }
    }
    metrics_.scan_frames = scan_frames;
    metrics_.foreground_measures +=
        file_metrics.space_measure_calls - metrics_.files.space_measure_calls;
    metrics_.foreground_measure_us +=
        file_metrics.space_measure_us - metrics_.files.space_measure_us;
    metrics_.files = file_metrics;
    portEXIT_CRITICAL(&mutex_);
    if (result.post()) {
        Host::wake(&owner_);
    }
    const auto post_us = static_cast<uint64_t>(esp_timer_get_time()) - refreshed;
    portENTER_CRITICAL(&mutex_);
    metrics_.post_us += post_us;
    portEXIT_CRITICAL(&mutex_);
}
void Host::Storage::stop() {
    if (worker_) {
        portENTER_CRITICAL(&mutex_);
        state_ = State::draining;
        portEXIT_CRITICAL(&mutex_);
        xTaskNotifyGive(worker_);
        xSemaphoreTake(done_, portMAX_DELAY);
        // On SMP, the done signal may precede the actual suspend/yield. eTaskGetState
        // reports running until the worker has left the other core; do not free early.
        while (eTaskGetState(worker_) != eSuspended) {
            vTaskDelay(1);
        }
        vTaskDelete(worker_);
        worker_ = nullptr;
        state_ = State::closed;
    }
    log_.close();
}
void Host::storage_memory_requirements(std::size_t &bytes, std::size_t &alignment) {
    bytes = sizeof(Storage);
    alignment = alignof(Storage);
}
StorageOpen Host::open_storage(const char *label, uint32_t capacity, bool initialize_blank) {
    if (!label || !label[0] || capacity == 0 || capacity > request_capacity_limit) {
        return {StorageStatus::invalid_argument, 0};
    }
    if (storage_) {
        return {StorageStatus::already_open, 0};
    }
    void *memory = heap_caps_aligned_alloc(alignof(Storage), sizeof(Storage),
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) {
        return {StorageStatus::no_memory, 0};
    }
    auto *candidate = new (memory) Storage(*this, initialize_blank);
    const auto opened = candidate->open(label, capacity);
    if (opened.status == StorageStatus::ok) {
        storage_ = candidate;
    } else {
        candidate->~Storage();
        heap_caps_free(memory);
    }
    return opened;
}
storage::FileValuePort Host::value_store() {
    return storage_ ? storage::FileValuePort{this, &Host::submit_value} : storage::FileValuePort{};
}
StorageMetrics Host::storage_metrics() const {
    return storage_ ? storage_->metrics() : StorageMetrics{};
}
uint64_t Host::append_durable_us(uint64_t seq) const {
    return storage_ ? storage_->append_durable_us(seq) : 0;
}
ayni_submit_result_t Host::submit_value(void *context, const storage::FileValueRequest *request) {
    auto *storage = static_cast<Host *>(context)->storage_;
    return storage && request ? storage->submit_value(*request) : AYNI_SUBMIT_REJECTED_NOT_RETAINED;
}
void Host::close_storage() {
    if (storage_) {
        storage_->stop();
        last_worker_stack_free_ = storage_->stack_free_bytes();
        storage_->~Storage();
        heap_caps_free(storage_);
        storage_ = nullptr;
    }
}
uint32_t Host::worker_stack_free_bytes() const {
    return storage_ ? storage_->stack_free_bytes() : last_worker_stack_free_;
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
    Storage *storage = static_cast<Host *>(ctx)->storage_;
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
    Storage *storage = static_cast<Host *>(ctx)->storage_;
    if (storage == nullptr || completion == nullptr || (buffer == nullptr && buffer_size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit({Request::Port::read_prefix, attempt, completion, from_commit_seq,
                            nullptr, buffer, buffer_size});
}

ayni_submit_result_t Host::storage_read_log(void *ctx, uint64_t cursor, uint8_t *buffer,
                                            uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                            ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_;
    if (storage == nullptr || completion == nullptr || (buffer == nullptr && buffer_size != 0U)) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit(
        {Request::Port::read_log, attempt, completion, cursor, nullptr, buffer, buffer_size});
}

ayni_submit_result_t Host::storage_fence_writers(void *ctx, uint32_t minimum_epoch,
                                                 ayni_attempt_ref_v1 attempt,
                                                 ayni_completion_t *completion) {
    Storage *storage = static_cast<Host *>(ctx)->storage_;
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
    Storage *storage = static_cast<Host *>(ctx)->storage_;
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
    Storage *storage = static_cast<Host *>(ctx)->storage_;
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
    Storage *storage = static_cast<Host *>(ctx)->storage_;
    if (storage == nullptr || completion == nullptr || writer_epoch == 0U) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    return storage->submit({Request::Port::trim, attempt, completion, commit_seq, nullptr, nullptr,
                            0, writer_epoch, transition_id});
}

ayni_status_t Host::storage_free_bytes(void *ctx, uint64_t *out_free_bytes) {
    Storage *storage = static_cast<Host *>(ctx)->storage_;
    if (out_free_bytes == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (storage == nullptr) {
        return AYNI_STORAGE_FAILED;
    }
    return storage->free_bytes(out_free_bytes);
}

} // namespace ayni::hosts::esp_idf
