#include "kernel/completion/completion_pool.h"
#include "foundation/attempt_identity.h"

#include "kernel/ingress/ingress_lock.h"

#include <new>

namespace ayni::kernel {
namespace {

bool defined_submit_result(ayni_submit_result_t result) {
    return result == AYNI_SUBMIT_REJECTED_NOT_RETAINED || result == AYNI_SUBMIT_ACCEPTED ||
           result == AYNI_SUBMIT_ACCEPT_UNKNOWN;
}

} // namespace

std::size_t CompletionPool::storage_size(uint32_t slot_count) {
    // Slots, then the free stack and the posted ring, one index each per slot.
    return sizeof(ayni_completion) * slot_count + 2U * sizeof(uint16_t) * slot_count;
}

CompletionPool::CompletionPool(void *storage, uint32_t slot_count, const ayni_host_v1 &host,
                               AttemptSequence &attempts)
    : host_(host), slots_(static_cast<ayni_completion *>(storage)),
      free_(reinterpret_cast<uint16_t *>(slots_ + slot_count)), posted_(free_ + slot_count),
      slot_count_(slot_count), free_count_(slot_count), attempts_(attempts) {
    for (uint32_t index = 0; index < slot_count; ++index) {
        new (&slots_[index])
            ayni_completion{this, {}, {}, nullptr, nullptr, index, SlotState::free};
        free_[index] = static_cast<uint16_t>(slot_count - 1U - index);
    }
}

IssueResult CompletionPool::issue(uint64_t boot_epoch, CompletionHandler handler,
                                  void *handler_context, PortCall call, void *call_context) {
    if (free_count_ == 0U) {
        const IngressGuard guard(host_);
        ++counters_.rejected_busy;
        return IssueResult::busy;
    }
    ayni_attempt_ref_v1 attempt{};
    if (!attempts_.next(boot_epoch, attempt)) {
        return IssueResult::exhausted;
    }
    ayni_completion &slot = slots_[free_[--free_count_]];
    {
        const IngressGuard guard(host_);
        slot.attempt = attempt;
        slot.handler = handler;
        slot.handler_context = handler_context;
        slot.state = SlotState::reserved;
    }

    const ayni_submit_result_t submitted = call(call_context, attempt, &slot);

    const IngressGuard guard(host_);
    if (!defined_submit_result(submitted)) {
        ++counters_.host_contract_violations;
        return IssueResult::in_flight;
    }
    if (submitted != AYNI_SUBMIT_REJECTED_NOT_RETAINED) {
        return IssueResult::in_flight;
    }
    if (slot.state == SlotState::posted) {
        // The Host posted a result and then denied keeping the work. The
        // result exists, so it is applied rather than discarded.
        ++counters_.host_contract_violations;
        return IssueResult::in_flight;
    }
    release(slot);
    return IssueResult::rejected;
}

uint32_t CompletionPool::apply_posted(uint32_t max_items) {
    uint32_t applied = 0;
    while (applied < max_items) {
        ayni_completion *slot = nullptr;
        {
            const IngressGuard guard(host_);
            if (posted_count_ == 0U) {
                break;
            }
            slot = &slots_[posted_[posted_head_]];
            posted_head_ = (posted_head_ + 1U) % slot_count_;
            --posted_count_;
        }
        // Posted slots are changed by no other thread, so the result is read
        // and handled outside the lock.
        slot->handler(slot->handler_context, slot->result);
        {
            const IngressGuard guard(host_);
            release(*slot);
        }
        ++applied;
    }
    return applied;
}

ayni_status_t CompletionPool::post(ayni_completion &slot, const ayni_completion_result_v1 &result) {
    {
        const IngressGuard guard(host_);
        if (slot.state != SlotState::reserved ||
            !foundation::same_attempt(slot.attempt, result.attempt)) {
            ++counters_.ignored_posts;
            return AYNI_INVALID_STATE;
        }
        if (result.outcome == 0U) {
            ++counters_.host_contract_violations;
        }
        slot.result = result;
        slot.state = SlotState::posted;
        posted_[(posted_head_ + posted_count_) % slot_count_] = static_cast<uint16_t>(slot.index);
        ++posted_count_;
    }
    host_.wake(host_.ctx);
    return AYNI_OK;
}

bool CompletionPool::owns(const ayni_completion *completion) const {
    return completion != nullptr && completion->index < slot_count_ &&
           &slots_[completion->index] == completion;
}

uint32_t CompletionPool::posted_count() const {
    const IngressGuard guard(host_);
    return posted_count_;
}

CompletionPool::Counters CompletionPool::counters() const {
    const IngressGuard guard(host_);
    return counters_;
}

// Caller holds the ingress lock.
void CompletionPool::release(ayni_completion &slot) {
    slot.state = SlotState::free;
    slot.handler = nullptr;
    slot.handler_context = nullptr;
    free_[free_count_++] = static_cast<uint16_t>(slot.index);
}

} // namespace ayni::kernel
