// Completion slots for asynchronous Host requests (C-HOST-006, C-HOST-018,
// ADR-0015).
//
// A slot is reserved before a port is called, so a result can always be
// posted, however full the event queue is. Each slot passes through
// free -> reserved -> posted -> free; every state change happens under the
// ingress lock because posts arrive from Host threads. A post is applied only
// if it names the attempt the slot is reserved for, so a late or repeated
// post can never complete a later attempt, and every accepted request has its
// handler called exactly once.

#ifndef AYNI_CORE_KERNEL_COMPLETION_COMPLETION_POOL_H
#define AYNI_CORE_KERNEL_COMPLETION_COMPLETION_POOL_H

#include "kernel/ports/attempt_sequence.h"

#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

class CompletionPool;

// Called on the owner thread when a posted result is applied. The slot is
// freed after the handler returns, so the handler may issue new requests.
using CompletionHandler = void (*)(void *context, const ayni_completion_result_v1 &result);

// A Host port call, made once a slot is reserved for the attempt.
using PortCall = ayni_submit_result_t (*)(void *context, ayni_attempt_ref_v1 attempt,
                                          ayni_completion_t *completion);

enum class IssueResult {
    busy,      // no free slot; the port was not called (C-HOST-006)
    rejected,  // the Host proved it kept no work; the handler will not run
    in_flight, // the handler runs once the Host posts the result
    exhausted, // request identity space ended; no port call or slot reservation
};

enum class SlotState : uint32_t { free, reserved, posted };

} // namespace ayni::kernel

// The ABI's opaque ayni_completion_t.
struct ayni_completion {
    ayni::kernel::CompletionPool *pool;
    ayni_attempt_ref_v1 attempt;
    ayni_completion_result_v1 result;
    ayni::kernel::CompletionHandler handler;
    void *handler_context;
    uint32_t index;
    ayni::kernel::SlotState state;
};

namespace ayni::kernel {

class CompletionPool {
  public:
    struct Counters {
        uint64_t rejected_busy; // issues refused for lack of a slot
        uint64_t ignored_posts; // posts for an attempt the slot is not waiting for
        // Unknown submit results, posts before a rejection, posts with outcome 0.
        uint64_t host_contract_violations;
    };

    static constexpr std::size_t storage_alignment = alignof(ayni_completion);
    static std::size_t storage_size(uint32_t slot_count);

    // storage holds storage_size(slot_count) bytes aligned to
    // storage_alignment; it and host outlive the pool. host must have wake and
    // both ingress lock functions. 1 <= slot_count <= 65535.
    CompletionPool(void *storage, uint32_t slot_count, const ayni_host_v1 &host,
                   AttemptSequence &attempts);

    CompletionPool(const CompletionPool &) = delete;
    CompletionPool &operator=(const CompletionPool &) = delete;

    // Owner thread. Reserves a slot, names the attempt (boot_epoch and the
    // next request_id) and calls the port with it. handler and call are
    // non-null. A submit result other than the three defined values is
    // treated as ACCEPT_UNKNOWN (C-HOST-004).
    IssueResult issue(uint64_t boot_epoch, CompletionHandler handler, void *handler_context,
                      PortCall call, void *call_context);

    // Owner thread. Applies at most max_items posted results in the order
    // they were posted and returns how many it applied.
    uint32_t apply_posted(uint32_t max_items);

    // Any thread except an ISR. AYNI_OK when stored, after which wake is
    // called; AYNI_INVALID_STATE, counted, when the slot is not reserved for
    // result.attempt. Outcome 0 is stored like any other and counted as a
    // Host defect, so the slot still completes.
    ayni_status_t post(ayni_completion &slot, const ayni_completion_result_v1 &result);

    // Whether completion points at one of this pool's slots.
    bool owns(const ayni_completion *completion) const;

    // Owner thread. Slots reserved or posted but not yet applied; STOPPED
    // requires 0 (C-HOST-018).
    uint32_t in_flight() const { return slot_count_ - free_count_; }

    // Owner thread. Whether issue() would find a slot.
    bool has_free_slot() const { return free_count_ > 0U; }

    // Owner thread. Posted results waiting for apply_posted.
    uint32_t posted_count() const;

    // Owner thread.
    Counters counters() const;

  private:
    void release(ayni_completion &slot);

    const ayni_host_v1 &host_;
    ayni_completion *slots_;
    uint16_t *free_;   // stack of free slot indices; changed by the owner only
    uint16_t *posted_; // ring of posted slot indices, in posting order
    uint32_t slot_count_;
    uint32_t free_count_;
    uint32_t posted_head_ = 0;
    uint32_t posted_count_ = 0;
    AttemptSequence &attempts_; // Borrowed Kernel identity owner, shared with Provider delivery.
    Counters counters_{};
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMPLETION_COMPLETION_POOL_H
