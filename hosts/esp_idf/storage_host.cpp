#include "littlefs_files.h"
#include <ayni/hosts/esp_idf.h>
#include <ayni/hosts/storage/request.h>
#include <ayni/storage_format.h>
#include <esp_heap_caps.h>
#include <new>
namespace ayni::hosts::esp_idf {
using storage::Request;
namespace {
constexpr TickType_t space_refresh_ticks = pdMS_TO_TICKS(1000);
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
    ayni_submit_result_t submit(const Request &request);
    void stop();
    ayni_status_t free_bytes(uint64_t *out);
    uint32_t stack_free_bytes() const {
        return worker_ ? uxTaskGetStackHighWaterMark(worker_) : final_stack_free_;
    }

  private:
    static void run(void *ctx);
    void serve_queue();
    bool refresh_space(uint64_t completed_reservation);
    Host &owner_;
    LittleFiles files_;
    storage::FileLog log_;
    std::array<Request, Host::request_capacity_limit> requests_{};
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
ayni_submit_result_t Host::Storage::submit(const Request &request) {
    bool accepted = false;
    portENTER_CRITICAL(&mutex_);
    if (state_ == State::running && count_ < capacity_) {
        requests_[(head_ + count_) % capacity_] = request;
        ++count_;
        reserved_ += storage::request_reservation(request);
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
bool Host::Storage::refresh_space(uint64_t completed_reservation) {
    uint64_t available = 0;
    const bool valid = log_.free_bytes(available);
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
void Host::Storage::serve_queue() {
    for (;;) {
        Request request{};
        bool has_request = false, finished = false;
        portENTER_CRITICAL(&mutex_);
        has_request = count_ != 0;
        finished = !has_request && state_ == State::draining;
        if (has_request) {
            request = requests_[head_];
            head_ = (head_ + 1) % capacity_;
            --count_;
        }
        portEXIT_CRITICAL(&mutex_);
        if (finished) {
            return;
        }
        if (!has_request) {
            if (refresh_space(0)) {
                Host::wake(&owner_);
            }
            ulTaskNotifyTake(pdTRUE, space_refresh_ticks);
            continue;
        }
        const auto result = storage::serve_request(log_, request);
        static_cast<void>(refresh_space(storage::request_reservation(request)));
        static_cast<void>(ayni_completion_post(request.completion, &result));
    }
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
