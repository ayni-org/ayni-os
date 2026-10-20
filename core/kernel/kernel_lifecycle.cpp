// Kernel lifecycle (contracts/domain/lifecycles.md, ADR-0016): recovery runs
// in bounded steps after ayni_start, entering RUNNING starts the work model
// (ADR-0017), steps take posted events (ADR-0021) and, in RUNNING, do the
// goals' due work (ADR-0018, ADR-0020) and have the strategies decide READY
// tasks (ADR-0022), and shutdown completes in a step once no completion slot
// is in flight (C-HOST-018).

#include "catalog/catalog_id.h"
#include "kernel/intent/decision.h"
#include "kernel/intent/work_model.h"
#include "kernel/kernel_state.h"

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
    // Registration is closed from here on, so each goal type's strategy is
    // resolved once and its index stays valid (C-LIFE-003).
    if (!work.goal_types.link_strategies(work.strategies)) {
        return AYNI_NOT_FOUND;
    }
    ingress.open();
    lifecycle = AYNI_LIFECYCLE_RECOVERING;
    return AYNI_OK;
}

// Each applied completion is one item and the clock is read after each; the
// boot sequence then issues at most one storage request, also one item. The
// request is never awaited: its result arrives in a later step. In RUNNING
// each piece of the goals' due work is then one item, then each decision,
// and after them each event queued when the step comes to the events, again
// with the clock read after each.
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
    // Due work is taken as of the start of the step, so one step sees one
    // schedule. It comes before events: a Host can keep the queue full, but
    // the due work at one time is finite, so neither starves the other.
    // Decisions likewise take only the tasks READY before the due work, so a
    // task derived in this step is decided in the next and a step does
    // finite work however the strategies answer.
    if (lifecycle == AYNI_LIFECYCLE_RUNNING) {
        const uint64_t ready_before = work.transitions.next_commit_seq();
        while (items < max_items && !out_of_time && ayni::kernel::do_due_work(work, started_us)) {
            ++items;
            out_of_time = time_is_up(host, started_us, max_time_us);
        }
        while (items < max_items && !out_of_time &&
               ayni::kernel::decide_next(work, decisions, ready_before, started_us)) {
            ++items;
            out_of_time = time_is_up(host, started_us, max_time_us);
        }
    }
    // Only the events already queued are taken; later ones wait for the next
    // step. An event's occurrences fall due at the step's start, and the
    // step reports the work they make due as ready.
    if (takes_events()) {
        uint32_t waiting = ingress.queued();
        ayni::catalog::CatalogId source;
        while (waiting > 0U && items < max_items && !out_of_time &&
               ingress.take_local_input(source)) {
            --waiting;
            ++items;
            handle_local_input(source, started_us);
            out_of_time = time_is_up(host, started_us, max_time_us);
        }
    }
    if (lifecycle == AYNI_LIFECYCLE_DRAINING && completions.in_flight() == 0U) {
        events_discarded += ingress.close();
        lifecycle = AYNI_LIFECYCLE_STOPPED;
    }

    // A request waiting for a slot is not ready: the post that frees one
    // wakes the owner.
    const bool boot_due = lifecycle == AYNI_LIFECYCLE_RECOVERING && boot.wants_advance() &&
                          completions.has_free_slot();
    // DRAINING and RESTRICTED do no due work and decide nothing (ADR-0018).
    // Runs after the time budget is spent too: more_ready must not drop due
    // work. A decision ends or holds its task, so a task still READY waits
    // for one.
    uint64_t next_deadline_us = ayni::work::no_time;
    const bool work_due = lifecycle == AYNI_LIFECYCLE_RUNNING &&
                          (ayni::kernel::refresh_due_work(work, started_us, next_deadline_us) ||
                           ayni::kernel::awaits_decision(work));
    report = ayni_step_report_v1{};
    report.struct_size = sizeof(report);
    report.items_done = items;
    const bool events_due = takes_events() && ingress.queued() > 0U;
    report.more_ready =
        completions.posted_count() > 0U || boot_due || events_due || work_due ? 1U : 0U;
    report.next_deadline_us = next_deadline_us;
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

// RECOVERING keeps events for RUNNING, when the goals they trigger exist.
// DRAINING and RESTRICTED make no new work (ADR-0018), so they discard them.
bool ayni_kernel::takes_events() const {
    return lifecycle == AYNI_LIFECYCLE_RUNNING || lifecycle == AYNI_LIFECYCLE_DRAINING ||
           lifecycle == AYNI_LIFECYCLE_RESTRICTED;
}

void ayni_kernel::handle_local_input(const ayni::catalog::CatalogId &source, uint64_t now_us) {
    if (lifecycle != AYNI_LIFECYCLE_RUNNING) {
        ++events_discarded;
    } else if (!ayni::kernel::apply_local_input(work, source, now_us)) {
        ++events_unmatched;
    }
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
