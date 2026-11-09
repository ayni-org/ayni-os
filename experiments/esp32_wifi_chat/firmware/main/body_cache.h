#pragma once
#include <ayni/runtime/conversation/file_bodies.h>
#include <memory>
#include <mutex>
namespace wifichat {
// Stable public descriptor over the SDK FileBodies. Only the owner may reset a
// quiescent cache between completed turns; disk bodies and references remain intact.
// The lock excludes API-worker reads while a drained backend is replaced.
class BodyCache {
  public:
    ayni_status_t configure(ayni::hosts::storage::FileValuePort files) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (backend_) {
            return AYNI_INVALID_STATE;
        }
        files_ = files;
        return create();
    }
    const ayni_body_store_v1 *store() const { return &store_; }
    static uint32_t ready(void *context, uint64_t task) {
        auto &self = *static_cast<BodyCache *>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        return self.drained(task);
    }
    void collect(uint64_t scope, const std::vector<ayni_body_ref_v1> &keep) {
        std::lock_guard<std::mutex> lock(mutex_);
        backend_->collect(scope, keep);
    }
    ayni_status_t reset_idle() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!drained(0)) {
            return AYNI_BUSY;
        }
        auto candidate = std::unique_ptr<ayni::runtime::FileBodies>(new (std::nothrow)
                                                                        ayni::runtime::FileBodies);
        if (!candidate) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        const auto status = candidate->configure({this, submit_file});
        if (status == AYNI_OK) {
            backend_.swap(candidate); // Drain proven; disk values are not removed.
        }
        return status;
    }

  private:
    ayni_status_t create() {
        backend_.reset(new (std::nothrow) ayni::runtime::FileBodies);
        return backend_ ? backend_->configure({this, submit_file}) : AYNI_CAPACITY_EXCEEDED;
    }
    uint32_t drained(uint64_t task) {
        refused_ = false;
        const auto ready = ayni::runtime::FileBodies::ready(backend_.get(), task);
        // FileBodies may have only refused removals left, which its ready count
        // does not include. Do not discard those unaccepted cleanup requests.
        return ready && !refused_;
    }
    static ayni_submit_result_t submit_file(void *context,
                                            const ayni::hosts::storage::FileValueRequest *request) {
        auto &self = *static_cast<BodyCache *>(context);
        const auto result = self.files_.submit(self.files_.context, request);
        self.refused_ = self.refused_ || result != AYNI_SUBMIT_ACCEPTED;
        return result;
    }
    static ayni_status_t write(void *context, uint64_t scope, ayni_bytes_v1 bytes,
                               ayni_body_ref_v1 *out) {
        auto &self = *static_cast<BodyCache *>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        const auto *port = self.backend_->store();
        return port->write(port->context, scope, bytes, out);
    }
    static ayni_status_t read(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                              uint32_t capacity) {
        auto &self = *static_cast<BodyCache *>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        const auto *port = self.backend_->store();
        return port->read(port->context, ref, out, capacity);
    }
    static void release(void *context, const ayni_body_ref_v1 *ref) {
        auto &self = *static_cast<BodyCache *>(context);
        std::lock_guard<std::mutex> lock(self.mutex_);
        const auto *port = self.backend_->store();
        port->release(port->context, ref);
    }
    mutable std::mutex mutex_;
    ayni::hosts::storage::FileValuePort files_{};
    std::unique_ptr<ayni::runtime::FileBodies> backend_;
    bool refused_ = false; // Owner-only, under mutex_; last flush must retain refused cleanup.
    ayni_body_store_v1 store_{sizeof(store_), 0, this, write, read, release};
};
} // namespace wifichat
