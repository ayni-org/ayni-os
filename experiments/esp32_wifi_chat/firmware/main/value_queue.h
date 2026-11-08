#pragma once
#include <array>
#include <atomic>
#include <ayni/hosts/storage/file_value.h>
namespace wifichat {
// Owner-thread outer-file admission, with worker-thread completions. The Host
// queue is also used by Core, so bulk body writes/removals must not fill it.
// This object and caller buffers outlive Host close/join; it never owns payloads.
class ValueQueue {
  public:
    static constexpr uint32_t host_capacity = 16, value_capacity = 8;
    bool configure(ayni::hosts::storage::FileValuePort underlying) {
        if (underlying_.submit || !underlying.submit) {
            return false;
        }
        underlying_ = underlying;
        return true;
    }
    ayni::hosts::storage::FileValuePort port() { return {this, submit}; }

  private:
    using Result = ayni::hosts::storage::FileValueResult;
    using Request = ayni::hosts::storage::FileValueRequest;
    struct Slot {
        std::atomic<bool> busy{false};
        void *context = nullptr;
        void (*complete)(void *, const Result &) noexcept = nullptr;
    };
    static void complete(void *context, const Result &result) noexcept {
        auto &slot = *static_cast<Slot *>(context);
        const auto callback = slot.complete;
        const auto caller = slot.context;
        // Copy before publishing the free slot: the owner may reuse it while
        // the original caller's callback is still executing on the worker.
        slot.busy.store(false, std::memory_order_release);
        callback(caller, result);
    }
    static ayni_submit_result_t submit(void *context, const Request *request) {
        auto &self = *static_cast<ValueQueue *>(context);
        if (!self.underlying_.submit || !request || !request->complete) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        for (auto &slot : self.slots_) {
            bool free = false;
            if (!slot.busy.compare_exchange_strong(free, true, std::memory_order_acq_rel)) {
                continue;
            }
            slot.context = request->context;
            slot.complete = request->complete;
            auto forwarded = *request;
            forwarded.context = &slot;
            forwarded.complete = complete;
            const auto accepted = self.underlying_.submit(self.underlying_.context, &forwarded);
            if (accepted != AYNI_SUBMIT_ACCEPTED) {
                slot.busy.store(false, std::memory_order_release);
            }
            return accepted;
        }
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::hosts::storage::FileValuePort underlying_{}; // Immutable after configure.
    std::array<Slot, value_capacity> slots_{};
};
} // namespace wifichat
