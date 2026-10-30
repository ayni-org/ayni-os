// One Task's durable operation frontier, with a boot-local finite timer.
#ifndef AYNI_CORE_WORK_CALL_OPERATION_WAIT_H
#define AYNI_CORE_WORK_CALL_OPERATION_WAIT_H
#include "foundation/kernel_id.h"
#include <ayni/operations.h>
namespace ayni::work {
// No frame history, result body or copied resource/lease ownership.
struct OperationWait {
    uint64_t call_id = 0;                        // Positive retained Call owned by this Task.
    uint64_t generation = 0;                     // Positive original operation/opening.
    uint64_t sequence = 0;                       // Last observed progress/sample; zero before any.
    uint32_t phase = AYNI_SUBSCRIPTION_STARTING; // Selected by form, never a lifecycle flag.
    uint32_t form =
        AYNI_CALL_SUBSCRIPTION;    // ACTION or SUBSCRIPTION; old native wait is SUBSCRIPTION.
    uint64_t boot_epoch = 0;       // Zero for legacy native subscription wait; otherwise positive.
    uint64_t gaps = 0;             // Latest diagnostic discontinuities; zero for action.
    uint32_t cancel_requested = 0; // 0/1, independent of terminal outcome.
    uint32_t cancel_result = 0;    // None, CANNOT_CANCEL or CANCEL_UNKNOWN.
    uint32_t outcome = 0;          // Terminal frontier; zero pending.
    uint32_t timeout_ms = 0;       // Zero only for native subscription wait; model wait is finite.
    uint64_t deadline_us =
        0; // Boot-local only, never encoded; zero interrupts a restored finite wait.
    // One representation validator for staged and restored frontiers.
    bool valid() const {
        return foundation::is_kernel_id(call_id) && foundation::is_kernel_id(generation) &&
               (form == AYNI_CALL_ACTION || form == AYNI_CALL_SUBSCRIPTION) &&
               (form == AYNI_CALL_ACTION
                    ? phase >= AYNI_CALL_QUEUED && phase <= AYNI_CALL_RESULT_UNKNOWN
                    : phase >= AYNI_SUBSCRIPTION_STARTING && phase <= AYNI_SUBSCRIPTION_FAILED) &&
               cancel_requested <= 1 &&
               (outcome == 0 || outcome == AYNI_CALL_COMPLETED || outcome == AYNI_CALL_FAILED ||
                outcome == AYNI_CALL_CANCELLED || outcome == AYNI_CALL_ABANDONED ||
                outcome == AYNI_CALL_SKIPPED) &&
               (cancel_result == 0 || cancel_result == AYNI_CALL_CANNOT_CANCEL ||
                cancel_result == AYNI_CALL_CANCEL_UNKNOWN) &&
               (timeout_ms == 0 ? form == AYNI_CALL_SUBSCRIPTION && boot_epoch == 0
                                : timeout_ms <= AYNI_OPERATION_WAIT_MS_MAX && boot_epoch > 0 &&
                                      boot_epoch <= foundation::max_boot_epoch);
    }
    // The same wire-independent frontier is used by staging and wake comparisons.
    ayni_operation_frontier_v1 frontier() const {
        return {boot_epoch, sequence, gaps, phase, cancel_requested, cancel_result, outcome};
    }
    // Equality describes durable facts; the restored local timer is intentionally absent.
    bool operator==(const OperationWait &other) const {
        return call_id == other.call_id && generation == other.generation &&
               sequence == other.sequence && phase == other.phase && form == other.form &&
               boot_epoch == other.boot_epoch && gaps == other.gaps &&
               cancel_requested == other.cancel_requested && cancel_result == other.cancel_result &&
               outcome == other.outcome && timeout_ms == other.timeout_ms;
    }
    bool operator!=(const OperationWait &other) const { return !(*this == other); }
};
} // namespace ayni::work
#endif
