// Bounded outer-file admission over a shared Host queue; deployment chooses its reserved room.
#ifndef AYNI_HOSTS_STORAGE_VALUE_ADMISSION_H
#define AYNI_HOSTS_STORAGE_VALUE_ADMISSION_H
#include <array>
#include <atomic>
#include <ayni/hosts/storage/file_value.h>
namespace ayni::hosts::storage {
// One submitting owner, worker-thread completions. Payloads remain caller-owned through
// completion/Host join. Slot storage is fixed; no queued payload or worker is created here.
// Configure before publishing port(); object and underlying port outlive accepted requests.
template <size_t Capacity> class ValueAdmission {
    static_assert(Capacity > 0, "Admission requires at least one slot");

  public:
    bool configure(FileValuePort underlying, size_t limit = Capacity) {
        if (underlying_.submit || !underlying.submit || !limit || limit > Capacity) {
            return false;
        }
        underlying_ = underlying;
        limit_ = limit;
        return true;
    }
    FileValuePort port() { return {this, submit}; }
    size_t pending() const {
        size_t count = 0;
        for (const auto &slot : slots_) {
            count += slot.busy.load(std::memory_order_acquire) ? 1U : 0U;
        }
        return count;
    }

  private:
    struct Slot {
        std::atomic<bool> busy{false};
        void *context = nullptr;
        void (*complete)(void *, const FileValueResult &) noexcept = nullptr;
    };
    static void complete(void *context, const FileValueResult &result) noexcept {
        auto &slot = *static_cast<Slot *>(context);
        const auto callback = slot.complete;
        const auto caller = slot.context;
        // Copy before publishing free: the owner may reuse this slot during the callback.
        slot.busy.store(false, std::memory_order_release);
        callback(caller, result);
    }
    static ayni_submit_result_t submit(void *context, const FileValueRequest *request) {
        auto &self = *static_cast<ValueAdmission *>(context);
        if (!self.underlying_.submit || !request || !request->complete) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (size_t at = 0; at < self.limit_; ++at) {
            auto &slot = self.slots_[at];
            bool free = false;
            if (!slot.busy.compare_exchange_strong(free, true, std::memory_order_acq_rel)) {
                continue;
            }
            slot.context = request->context;
            slot.complete = request->complete;
            auto forwarded = *request;
            forwarded.context = &slot;
            forwarded.complete = complete;
            const auto result = self.underlying_.submit(self.underlying_.context, &forwarded);
            if (result != AYNI_SUBMIT_ACCEPTED) {
                slot.busy.store(false, std::memory_order_release);
            }
            return result;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    FileValuePort underlying_{};
    size_t limit_ = 0;
    std::array<Slot, Capacity> slots_{};
};
} // namespace ayni::hosts::storage
#endif
