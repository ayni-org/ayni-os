#include "kernel/completion/executor_mailbox.h"
#include "foundation/attempt_identity.h"
#include "kernel/ingress/ingress_lock.h"

#include <new>

namespace ayni::kernel {
std::size_t ExecutorMailbox::storage_size(uint32_t executors) {
    return sizeof(Lane) * lane_count * executors;
}

ExecutorMailbox::ExecutorMailbox(void *storage, uint32_t executors, const ayni_host_v1 &host)
    : host_(host), lanes_(static_cast<Lane *>(storage)), executors_(executors) {
    static_assert(alignof(Lane) <= storage_alignment, "caller arena aligns lanes");
    for (uint32_t executor = 0; executor < executors_; ++executor) {
        for (uint32_t lane = 0; lane < lane_count; ++lane) {
            new (&lanes_[executor * lane_count + lane]) Lane{{this, executor, lane}, {}, {}};
        }
    }
}

ExecutorMailbox::Lane &ExecutorMailbox::at(uint32_t executor, ExecutorLane lane) {
    return lanes_[executor * lane_count + static_cast<uint32_t>(lane)];
}

const ExecutorMailbox::Lane &ExecutorMailbox::at(uint32_t executor, ExecutorLane lane) const {
    return lanes_[executor * lane_count + static_cast<uint32_t>(lane)];
}

ayni_executor_completion_t *ExecutorMailbox::open(uint32_t executor, ExecutorLane lane,
                                                  const ExecutorRequest &request) {
    auto &slot = at(executor, lane);
    const IngressGuard guard(host_);
    if (slot.request) {
        return nullptr;
    }
    slot.request = request;
    slot.answer.reset();
    return &slot.target;
}

const ExecutorRequest *ExecutorMailbox::request(uint32_t executor, ExecutorLane lane) const {
    const auto &slot = at(executor, lane);
    return slot.request ? &*slot.request : nullptr;
}

bool ExecutorMailbox::answer(uint32_t executor, ExecutorLane lane, ExecutorAnswer &out) const {
    const auto &slot = at(executor, lane);
    const IngressGuard guard(host_);
    if (!slot.request || !slot.answer) {
        return false;
    }
    out = *slot.answer;
    return true;
}

void ExecutorMailbox::refuse(uint32_t executor, ExecutorLane lane, ayni_status_t status) {
    auto &slot = at(executor, lane);
    const IngressGuard guard(host_);
    if (slot.request) {
        slot.answer = ExecutorAnswer{status, {}};
    }
}

void ExecutorMailbox::close(uint32_t executor, ExecutorLane lane) {
    auto &slot = at(executor, lane);
    const IngressGuard guard(host_);
    slot.request.reset();
    slot.answer.reset();
}

// Only the owner writes requests, so it reads them without the lock; whether
// an answer has arrived is read under it.
uint64_t ExecutorMailbox::next_deadline_us() const {
    uint64_t next = UINT64_MAX;
    const IngressGuard guard(host_);
    for (uint32_t at = 0; at < executors_ * lane_count; ++at) {
        const auto &slot = lanes_[at];
        if (slot.request && !slot.answer && slot.request->deadline_us < next) {
            next = slot.request->deadline_us;
        }
    }
    return next;
}

bool ExecutorMailbox::idle() const {
    for (uint32_t at = 0; at < executors_ * lane_count; ++at) {
        if (lanes_[at].request) {
            return false;
        }
    }
    return true;
}

bool ExecutorMailbox::owns(const ayni_executor_completion_t *target) const {
    return target != nullptr && target->mailbox == this && target->executor < executors_ &&
           target->lane < lane_count &&
           &lanes_[target->executor * lane_count + target->lane].target == target;
}

// Copy under the ingress lock, then wake outside it. The output arm is kept
// only for the kinds that carry one and only with OK; the owner validates it.
ayni_status_t ExecutorMailbox::post(const ayni_executor_completion_t &target,
                                    const ayni_executor_response_v1 &response) {
    if (response.kind < AYNI_EXECUTOR_CHALLENGE || response.kind > AYNI_EXECUTOR_STATUS ||
        response.reserved != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    const bool carries = response.status == AYNI_OK && (response.kind == AYNI_EXECUTOR_CHALLENGE ||
                                                        response.kind == AYNI_EXECUTOR_STATUS);
    ExecutorAnswer answer{response.status, {}};
    if (carries) {
        answer.output = response.output;
    }
    {
        auto &slot = at(target.executor, static_cast<ExecutorLane>(target.lane));
        const IngressGuard guard(host_);
        if (!slot.request || slot.answer || slot.request->kind != response.kind ||
            !foundation::same_attempt(slot.request->identity, response.request)) {
            return AYNI_INVALID_STATE;
        }
        slot.answer = answer;
    }
    host_.wake(host_.ctx);
    return AYNI_OK;
}
} // namespace ayni::kernel
