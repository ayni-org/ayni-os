// Kernel lifecycle (contracts/domain/lifecycles.md, ADR-0016): recovery runs
// in bounded steps after ayni_start, entering RUNNING starts the work model
// (ADR-0017), RUNNING derives tasks in steps (ADR-0018), and shutdown
// completes in a step once no completion slot is in flight (C-HOST-018).

#include "kernel/intent/work_model.h"
#include "kernel/kernel_state.h"

#include <limits>

namespace {

bool time_is_up(const ayni_host_v1 &host, uint64_t started_us, uint32_t max_time_us) {
    return host.monotonic_now_us(host.ctx) - started_us >= max_time_us;
}

} // namespace

ayni_status_t ayni_kernel::start() {
    if (lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    // ayni_create accepts both storage ports or neither (C-HOST-015).
    if (host.storage_append == nullptr) {
        return AYNI_UNSUPPORTED;
    }
    lifecycle = AYNI_LIFECYCLE_RECOVERING;
    return AYNI_OK;
}

// Each applied completion is one item and the clock is read after each; the
// boot sequence then issues at most one storage request, also one item. The
// request is never awaited: its result arrives in a later step. In RUNNING,
// each derived task is then one item, again with the clock read after each.
ayni_status_t ayni_kernel::step(uint32_t max_items, uint32_t max_time_us,
                                ayni_step_report_v1 &report) {
    if (lifecycle == AYNI_LIFECYCLE_CONFIGURING || lifecycle == AYNI_LIFECYCLE_STOPPED) {
        return AYNI_INVALID_STATE;
    }
    const uint64_t started_us = host.monotonic_now_us(host.ctx);
    uint32_t items = 0;
    bool out_of_time = false;
    while (items < max_items && !out_of_time && completions.apply_posted(1) == 1U) {
        ++items;
        out_of_time = time_is_up(host, started_us, max_time_us);
    }
    settle_recovery();
    if (lifecycle == AYNI_LIFECYCLE_RECOVERING && items < max_items && !out_of_time &&
        boot.advance()) {
        ++items;
        settle_recovery();
    }
    if (lifecycle == AYNI_LIFECYCLE_RUNNING) {
        while (items < max_items && !out_of_time && ayni::kernel::derive_next_task(work)) {
            ++items;
            out_of_time = time_is_up(host, started_us, max_time_us);
        }
    }
    if (lifecycle == AYNI_LIFECYCLE_DRAINING && completions.in_flight() == 0U) {
        lifecycle = AYNI_LIFECYCLE_STOPPED;
    }

    // A request waiting for a slot is not ready: the post that frees one
    // wakes the owner.
    const bool boot_due = lifecycle == AYNI_LIFECYCLE_RECOVERING && boot.wants_advance() &&
                          completions.has_free_slot();
    // DRAINING and RESTRICTED derive nothing (ADR-0018).
    const bool derivation_due =
        lifecycle == AYNI_LIFECYCLE_RUNNING && ayni::kernel::has_derivable_task(work);
    report = ayni_step_report_v1{};
    report.struct_size = sizeof(report);
    report.items_done = items;
    report.more_ready = completions.posted_count() > 0U || boot_due || derivation_due ? 1U : 0U;
    report.next_deadline_us = std::numeric_limits<uint64_t>::max();
    return AYNI_OK;
}

ayni_status_t ayni_kernel::begin_shutdown() {
    switch (lifecycle) {
    case AYNI_LIFECYCLE_CONFIGURING:
        return AYNI_INVALID_STATE;
    case AYNI_LIFECYCLE_DRAINING:
    case AYNI_LIFECYCLE_STOPPED:
        return AYNI_OK;
    default:
        // A boot request in flight still completes; no new one is issued.
        lifecycle = AYNI_LIFECYCLE_DRAINING;
        return AYNI_OK;
    }
}

ayni_restriction_t ayni_kernel::restriction() const {
    return lifecycle == AYNI_LIFECYCLE_RESTRICTED ? boot.restriction() : AYNI_RESTRICTION_NONE;
}

void ayni_kernel::settle_recovery() {
    if (lifecycle != AYNI_LIFECYCLE_RECOVERING) {
        return;
    }
    switch (boot.outcome()) {
    case ayni::kernel::BootSequence::Outcome::epoch_durable:
        enter_running();
        break;
    case ayni::kernel::BootSequence::Outcome::restricted:
        lifecycle = AYNI_LIFECYCLE_RESTRICTED;
        break;
    case ayni::kernel::BootSequence::Outcome::pending:
        break;
    }
}

// Transitions follow the boot batch, and the work model starts before any
// intent is accepted.
void ayni_kernel::enter_running() {
    const ayni::durability::BatchIdentity boot_batch = boot.boot_batch();
    work.start(boot.boot_epoch(), {boot_batch.transition_id + 1U, boot_batch.commit_seq + 1U});
    lifecycle = AYNI_LIFECYCLE_RUNNING;
}
