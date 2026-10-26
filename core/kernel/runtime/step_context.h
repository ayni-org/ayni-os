// One bounded step's clock, work accounting and report deadline (ADR-0058).
// This value never survives its call; scheduling uses the fixed entry instant,
// while authority checks read the borrowed clock again after caller code runs.
#ifndef AYNI_CORE_KERNEL_RUNTIME_STEP_CONTEXT_H
#define AYNI_CORE_KERNEL_RUNTIME_STEP_CONTEXT_H

#include "kernel/service/model/context.h"
#include "work/goal/trigger_schedule.h"

namespace ayni::kernel {

struct StepContext {
    // All budgets are explicit; the ABI has already rejected zero budgets.
    StepContext(uint64_t started, uint32_t item_budget, uint32_t time_budget, OwnerClock clock)
        : started_us(started), max_items(item_budget), max_time_us(time_budget), clock_(clock) {}

    const uint64_t started_us;  // Monotonic entry time in us; fixed schedule/READY/report frontier.
    const uint32_t max_items;   // Positive item limit; includes storage completions and owner work.
    const uint32_t max_time_us; // Positive elapsed-time budget; never an absolute timestamp.
    uint64_t next_deadline_us = work::no_time; // Report deadline; no_time means none scheduled.

    // Storage admission is separate; this only checks this call's remaining budget.
    bool available() const { return items_ < max_items && !out_of_time_; }
    // Fresh monotonic sample for dispatch/authority checks, never a schedule frontier.
    uint64_t now_us() const { return clock_.now_us(); }
    // Count one applied item and observe elapsed time before another loop iteration.
    void applied() {
        ++items_;
        out_of_time_ = now_us() - started_us >= max_time_us;
    }
    // Boot/maintenance advance once with their original no-extra-sample accounting.
    void storage_advanced() { ++items_; }
    // Completed items, always bounded by max_items; report remains available when exhausted.
    uint32_t items_done() const { return items_; }
    // Only future deadlines belong in the report; ready work uses more_ready instead.
    void include_deadline(uint64_t deadline_us) {
        if (deadline_us > started_us && deadline_us < next_deadline_us) {
            next_deadline_us = deadline_us;
        }
    }

  private:
    OwnerClock clock_;         // Borrowed Host clock, alive throughout this synchronous owner call.
    uint32_t items_ = 0;       // Applied items in this call; zero at entry.
    bool out_of_time_ = false; // Sticky once an applied item observes time-budget exhaustion.
};

} // namespace ayni::kernel
#endif
