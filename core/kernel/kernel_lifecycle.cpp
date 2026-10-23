// Kernel lifecycle (contracts/domain/lifecycles.md, ADR-0016): recovery runs
// in bounded steps after ayni_start, entering RUNNING starts the work model
// (ADR-0017), steps take posted events (ADR-0021) and, in RUNNING, do the
// goals' due work (ADR-0018, ADR-0020) and have the strategies decide READY
// tasks (ADR-0022), and shutdown completes in a step once no completion slot
// is in flight (C-HOST-018).
//
// Storage upkeep (ADR-0027) runs in the same steps. A RUNNING kernel whose
// log nears its limit compacts it into a snapshot, and a recovery that read
// a schema 0 log migrates it; either way StorageMaintenance holds the
// storage buffer and no transition starts until it is done. A writer whose
// retries run out is fenced: FENCED isolates the old writer, then recovery
// runs again on what is durable and RUNNING resumes under a new boot epoch.

#include "catalog/catalog_id.h"
#include "kernel/intent/authority_intent.h"
#include "kernel/intent/decision.h"
#include "kernel/intent/work_model.h"
#include "kernel/kernel_state.h"

// One step's local budget and scheduling instant; never retained by Kernel.
// Item application observes elapsed time, while boot/maintenance keep their
// existing one-shot accounting without an additional clock sample (ADR-0035).
struct ayni_kernel::StepBudget {
    const uint64_t started_us;  // This boot's monotonic start, shared by all due-work decisions.
    const uint32_t max_items;   // Maximum items requested for this call; positive at the ABI.
    const uint32_t max_time_us; // Elapsed-time budget in microseconds; positive at the ABI.
    uint32_t items = 0;         // Completed items in this call, never above max_items.
    bool out_of_time = false;   // True after an item observes the elapsed budget exhausted.

    // Storage has its own admission checks; this answers only the call budget.
    bool available() const { return items < max_items && !out_of_time; }
    // Count one applied item and stop subsequent bounded loops if time is up.
    void applied(const ayni_host_v1 &ports) {
        ++items;
        out_of_time = ports.monotonic_now_us(ports.ctx) - started_us >= max_time_us;
    }
};

// Close registration only after all storage ports and strategy links are ready;
// recovery publishes RUNNING only after the boot epoch becomes durable.
ayni_status_t ayni_kernel::start() {
    if (lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return AYNI_INVALID_STATE;
    }
    // ayni_create accepts both storage ports or neither (C-HOST-015).
    if (host.storage_append == nullptr || host.storage_read_prefix == nullptr ||
        host.storage_read_log == nullptr || host.storage_fence_writers == nullptr ||
        host.storage_write_snapshot == nullptr || host.storage_read_snapshot == nullptr ||
        host.storage_trim_prefix == nullptr || host.storage_free_bytes == nullptr) {
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
// boot sequence, or else a maintenance run, then takes at most one step of
// its own, also one item. A storage request is never awaited: its result
// arrives in a later step. With a writable journal, accepted revocations
// take one item each before ordinary work, also while DRAINING. In RUNNING,
// each revocation cleanup candidate then takes one item before goals' due
// work, decisions and events. Each piece of due work takes one item, then
// each decision, and after them each event queued when
// the step comes to the events, again with the clock read after each; each
// of them is one transition, which needs room for its batch first
// (ADR-0026). Last, the log issues the append of the oldest batch waiting,
// whatever is left of the budget: it is one call that does not wait, and
// holding it back would only delay durability.
ayni_status_t ayni_kernel::step(uint32_t max_items, uint32_t max_time_us,
                                ayni_step_report_v1 &report) {
    if (lifecycle == AYNI_LIFECYCLE_CONFIGURING || lifecycle == AYNI_LIFECYCLE_STOPPED) {
        return AYNI_INVALID_STATE;
    }
    StepBudget progress{host.monotonic_now_us(host.ctx), max_items, max_time_us};
    while (progress.available() && completions.apply_posted(1) == 1U) {
        progress.applied(host);
    }
    settle_recovery();
    settle_log();
    settle_maintenance();
    refresh_storage();
    advance_recovery_and_maintenance(progress);
    apply_revocations(progress);
    apply_ready_work(progress);
    take_queued_inputs(progress);
    if (appends() && log.advance(host.monotonic_now_us(host.ctx))) {
        settle_log();
        if (!retire_durable_revocations()) {
            enter_restricted(AYNI_RESTRICTION_INTERNAL);
        }
    }
    if (lifecycle == AYNI_LIFECYCLE_DRAINING && completions.in_flight() == 0U &&
        (!appends() || log.idle() || log.failure() != AYNI_RESTRICTION_NONE) &&
        !revocation_ready()) {
        events_discarded += ingress.close();
        lifecycle = AYNI_LIFECYCLE_STOPPED;
    }

    report_step(progress, report);
    return AYNI_OK;
}

// Recovery and maintenance share the storage buffer and each advance at most
// once per call. A completed migration can hand control back to recovery.
void ayni_kernel::advance_recovery_and_maintenance(StepBudget &progress) {
    // A migration interrupts recovery, which resumes once it is done.
    if ((lifecycle == AYNI_LIFECYCLE_RECOVERING || lifecycle == AYNI_LIFECYCLE_FENCED) &&
        progress.available() && !maintaining() && boot.advance(host.monotonic_now_us(host.ctx))) {
        ++progress.items;
        settle_recovery();
    }
    if ((lifecycle == AYNI_LIFECYCLE_RECOVERING || lifecycle == AYNI_LIFECYCLE_RUNNING) &&
        maintaining() && progress.available() &&
        maintenance.advance(host.monotonic_now_us(host.ctx))) {
        ++progress.items;
        settle_maintenance();
    }
}

// C-AUTH-026/027: accepted revocations precede new work, also while draining;
// descendant cleanup only advances while RUNNING. Journal each accepted change.
void ayni_kernel::apply_revocations(StepBudget &progress) {
    if (appends()) {
        if (!retire_durable_revocations()) {
            enter_restricted(AYNI_RESTRICTION_INTERNAL);
        }
        while ((lifecycle == AYNI_LIFECYCLE_RUNNING || lifecycle == AYNI_LIFECYCLE_DRAINING) &&
               appends() && progress.available() && has_room()) {
            const uint64_t first_seq = work.transitions.next_commit_seq();
            bool applied = ayni::kernel::apply_pending_revoke(work);
            if (!applied && lifecycle == AYNI_LIFECYCLE_RUNNING) {
                applied = ayni::kernel::apply_revocation_cleanup(work);
            }
            if (work.transitions.next_commit_seq() != first_seq) {
                if (!applied) {
                    enter_restricted(AYNI_RESTRICTION_INTERNAL);
                    break;
                }
                journal(first_seq);
                progress.applied(host);
            } else {
                break;
            }
        }
    }
}

// Preserve one schedule/READY frontier for this call so newly derived tasks
// wait until the next decision pass, independent of a strategy's answers.
void ayni_kernel::apply_ready_work(StepBudget &progress) {
    // Due work is taken as of the start of the step, so one step sees one
    // schedule. It comes before events: a Host can keep the queue full, but
    // the due work at one time is finite, so neither starves the other.
    // Decisions likewise take only the tasks READY before the due work, so a
    // task derived in this step is decided in the next and a step does
    // finite work however the strategies answer. A transition that
    // journal() restricts on ends both loops: nothing after it is stored.
    if (lifecycle == AYNI_LIFECYCLE_RUNNING) {
        const uint64_t ready_before = work.transitions.next_commit_seq();
        while (lifecycle == AYNI_LIFECYCLE_RUNNING && progress.available() && has_room()) {
            const uint64_t first_seq = work.transitions.next_commit_seq();
            if (!ayni::kernel::do_due_work(work, progress.started_us,
                                           host.monotonic_now_us(host.ctx), log.durable_seq())) {
                break;
            }
            journal(first_seq);
            progress.applied(host);
        }
        while (lifecycle == AYNI_LIFECYCLE_RUNNING && progress.available() && has_room()) {
            const uint64_t first_seq = work.transitions.next_commit_seq();
            if (!ayni::kernel::decide_next(work, decisions, ready_before, progress.started_us,
                                           log.durable_seq(), {host.ctx, host.monotonic_now_us})) {
                break;
            }
            journal(first_seq);
            progress.applied(host);
        }
    }
}

// Consume a bounded snapshot of the queue, preserving work-before-input order.
void ayni_kernel::take_queued_inputs(StepBudget &progress) {
    // Only the events already queued are taken; later ones wait for the next
    // step. An event's occurrences fall due at the step's start, and the
    // step reports the work they make due as ready.
    if (takes_events()) {
        uint32_t waiting = ingress.queued();
        ayni::catalog::CatalogId source;
        while (waiting > 0U && progress.available() && takes_event_now() &&
               ingress.take_local_input(source)) {
            --waiting;
            handle_local_input(source, progress.started_us);
            progress.applied(host);
        }
    }
}

// Report all readiness even when this call spent its budget: a false idle
// would lose the owner's next wakeup. Slot and storage backpressure still apply.
void ayni_kernel::report_step(const StepBudget &progress, ayni_step_report_v1 &report) {
    // A request waiting for a slot is not ready: the post that frees one
    // wakes the owner.
    const bool boot_due =
        (lifecycle == AYNI_LIFECYCLE_RECOVERING || lifecycle == AYNI_LIFECYCLE_FENCED) &&
        !maintaining() && boot.wants_advance(progress.started_us) && completions.has_free_slot();
    const bool log_due =
        appends() && log.wants_advance(progress.started_us) && completions.has_free_slot();
    // DRAINING and RESTRICTED do no due work and decide nothing (ADR-0018).
    // Runs after the time budget is spent too: more_ready must not drop due
    // work. A task still READY waits for a decision, also one that a
    // decision reporting progress left READY for a later step (ADR-0024).
    // Without room, all of it waits for an append's post, which wakes the
    // owner.
    uint64_t next_deadline_us = ayni::work::no_time;
    const bool work_due =
        lifecycle == AYNI_LIFECYCLE_RUNNING && has_room() &&
        (ayni::kernel::refresh_due_work(work, progress.started_us, next_deadline_us) ||
         ayni::kernel::awaits_decision(work));
    const bool maintenance_due = maintaining() && maintenance.wants_advance(progress.started_us) &&
                                 completions.has_free_slot();
    // A request held back by the write interval or a retry backoff is due
    // later; one due already is in more_ready instead.
    const uint64_t storage_due_us = storage_deadline();
    if (storage_due_us > progress.started_us && storage_due_us < next_deadline_us) {
        next_deadline_us = storage_due_us;
    }
    report = ayni_step_report_v1{};
    report.struct_size = sizeof(report);
    report.items_done = progress.items;
    const bool events_due = takes_events() && takes_event_now() && ingress.queued() > 0U;
    report.more_ready = completions.posted_count() > 0U || boot_due || log_due || maintenance_due ||
                                events_due || work_due || revocation_ready()
                            ? 1U
                            : 0U;
    report.next_deadline_us = next_deadline_us;
    report.durable_seq = durable_seq();
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
        // The batches RUNNING committed are still appended (appends()).
        lifecycle = AYNI_LIFECYCLE_DRAINING;
        return AYNI_OK;
    }
}

ayni_restriction_t ayni_kernel::restriction() const {
    return lifecycle == AYNI_LIFECYCLE_RESTRICTED || lifecycle == AYNI_LIFECYCLE_FENCED
               ? restriction_
               : AYNI_RESTRICTION_NONE;
}

// A reload starts recovery over, which counts again from the snapshot or
// the start of the log. durable_seq() must not fall meanwhile, so the value
// at the fence stays the floor.
uint64_t ayni_kernel::durable_seq() const {
    const uint64_t known = log_started_ ? log.durable_seq() : boot.durable_seq();
    return known > durable_floor_ ? known : durable_floor_;
}

bool ayni_kernel::maintaining() const {
    return maintenance.mode() != ayni::kernel::StorageMaintenance::Mode::none;
}

// Whichever owner issues the next storage request in this state.
uint64_t ayni_kernel::storage_deadline() const {
    switch (lifecycle) {
    case AYNI_LIFECYCLE_RECOVERING:
    case AYNI_LIFECYCLE_FENCED:
        return maintaining() ? maintenance.next_deadline() : boot.next_deadline();
    case AYNI_LIFECYCLE_RUNNING:
        return maintaining() ? maintenance.next_deadline() : log.next_deadline();
    case AYNI_LIFECYCLE_DRAINING:
        return appends() ? log.next_deadline() : ayni::work::no_time;
    default:
        return ayni::work::no_time;
    }
}

bool ayni_kernel::storage_has_room() const {
    return budget.admits() && !maintaining() && log.has_room();
}

ayni_status_t ayni_kernel::storage_admission_status() const {
    return budget.admission(host);
}

void ayni_kernel::journal(uint64_t first_seq) {
    const uint64_t next_seq = work.transitions.next_commit_seq();
    if (next_seq == first_seq ||
        (lifecycle != AYNI_LIFECYCLE_RUNNING && lifecycle != AYNI_LIFECYCLE_DRAINING)) {
        return;
    }
    // One call applies one transition, and only with room for its batch; a
    // batch that does not fit is a defect, which the log must not hide.
    const ayni::foundation::TransitionStamp stamp = work.transitions.latest();
    if (next_seq - first_seq != 1U || !log.has_room()) {
        enter_restricted(AYNI_RESTRICTION_INTERNAL);
        return;
    }
    ayni::durability::BatchWriter writer = log.begin();
    const std::size_t size = ayni::kernel::encode_transition(work, stamp, writer);
    if (size == 0U) {
        enter_restricted(AYNI_RESTRICTION_INTERNAL);
        return;
    }
    log.queue(size);
    budget.appended(size);
    refresh_storage();
}

void ayni_kernel::enter_restricted(ayni_restriction_t reason) {
    lifecycle = AYNI_LIFECYCLE_RESTRICTED;
    restriction_ = reason;
}

// RESTRICTED stores nothing more, also once it drains; the batches still
// waiting then never reach the log, as when an append fails.
bool ayni_kernel::appends() const {
    return log_started_ && restriction_ == AYNI_RESTRICTION_NONE &&
           (lifecycle == AYNI_LIFECYCLE_RUNNING || lifecycle == AYNI_LIFECYCLE_DRAINING);
}

void ayni_kernel::settle_log() {
    if (lifecycle == AYNI_LIFECYCLE_RUNNING && log.failure() != AYNI_RESTRICTION_NONE) {
        enter_fenced(log.failure());
    }
}

// RECOVERING keeps events for RUNNING, when the goals they trigger exist.
// DRAINING, RESTRICTED and FENCED make no new work (ADR-0018), so they
// discard them.
bool ayni_kernel::takes_events() const {
    return lifecycle == AYNI_LIFECYCLE_RUNNING || lifecycle == AYNI_LIFECYCLE_DRAINING ||
           lifecycle == AYNI_LIFECYCLE_RESTRICTED || lifecycle == AYNI_LIFECYCLE_FENCED;
}

// In RUNNING an event may be one transition, so it waits for room.
bool ayni_kernel::takes_event_now() const {
    return lifecycle != AYNI_LIFECYCLE_RUNNING || has_room();
}

void ayni_kernel::handle_local_input(const ayni::catalog::CatalogId &source, uint64_t now_us) {
    if (lifecycle != AYNI_LIFECYCLE_RUNNING) {
        ++events_discarded;
        return;
    }
    const uint64_t first_seq = work.transitions.next_commit_seq();
    if (!ayni::kernel::apply_local_input(work, source, now_us)) {
        ++events_unmatched;
    }
    journal(first_seq);
}

// FENCED recovers too: its boot sequence first isolates the old writer.
void ayni_kernel::settle_recovery() {
    if (lifecycle != AYNI_LIFECYCLE_RECOVERING && lifecycle != AYNI_LIFECYCLE_FENCED) {
        return;
    }
    switch (boot.outcome()) {
    case ayni::kernel::BootSequence::Outcome::epoch_durable:
        enter_running();
        break;
    case ayni::kernel::BootSequence::Outcome::restricted:
        // No part of a log recovery could not replay is visible (ADR-0026).
        // A fence that could not isolate the old writer replayed nothing,
        // and the model it keeps stays readable for diagnosis.
        if (boot.restriction() == AYNI_RESTRICTION_OWNER_BINDING_MISMATCH) {
            // The prefix was validated completely. A Host binding mismatch
            // cannot authorize it, but does not erase its diagnostic state.
            replay.finish(host.monotonic_now_us(host.ctx));
        } else if (lifecycle == AYNI_LIFECYCLE_RECOVERING) {
            replay.discard();
        }
        enter_restricted(boot.restriction());
        break;
    case ayni::kernel::BootSequence::Outcome::writers_isolated:
        // The old writer can no longer store anything. The model is dropped
        // only once every request is complete, since a Host may still read
        // the buffers they borrowed (ADR-0027 decision 14); recovery then
        // reads what is durable, under the epoch the fence installed.
        if (completions.in_flight() == 0U) {
            const uint32_t epoch = boot.writer_epoch();
            replay.discard_for_reload();
            log_started_ = false;
            boot.reset(epoch);
            lifecycle = AYNI_LIFECYCLE_RECOVERING;
        }
        break;
    case ayni::kernel::BootSequence::Outcome::migration_ready:
        if (!maintaining()) {
            begin_maintenance(true);
        }
        break;
    case ayni::kernel::BootSequence::Outcome::pending:
        break;
    }
}

// Transitions follow the boot batch, and the work model starts before any
// intent is accepted. The log takes the storage buffer over from recovery.
void ayni_kernel::enter_running() {
    if (!replay.complete() || !replay.consistent()) {
        replay.discard();
        enter_restricted(AYNI_RESTRICTION_LOG_CORRUPT);
        return;
    }
    replay.finish(host.monotonic_now_us(host.ctx));
    const ayni::durability::BatchIdentity boot_batch = boot.boot_batch();
    const ayni::foundation::TransitionStamp first{boot_batch.transition_id + 1U,
                                                  boot_batch.commit_seq + 1U};
    maintenance.adopt(boot.generation(), boot.active_slot());
    budget.restart(boot.log_bytes());
    restriction_ = AYNI_RESTRICTION_NONE;
    log.start(boot.boot_epoch(), {boot_batch.writer_epoch, first.commit_seq, first.transition_id},
              batch_max);
    log_started_ = true;
    lifecycle = AYNI_LIFECYCLE_RUNNING;
    if (!reconcile_recovered_revocations()) {
        enter_restricted(AYNI_RESTRICTION_LOG_CORRUPT);
        return;
    }
    if (work.start(boot.boot_epoch(), first)) {
        journal(first.commit_seq);
    }
}

// Only a RUNNING kernel is fenced, so its writer is the log's. The fence
// installs an epoch above it, and a writer that has none left restricts.
// Isolation and recovery failing again restrict too (settle_recovery()):
// one fence never leads to another before RUNNING (ADR-0027 decision 5).
void ayni_kernel::enter_fenced(ayni_restriction_t reason) {
    if (log.writer_epoch() == UINT32_MAX) {
        enter_restricted(AYNI_RESTRICTION_WRITER_EPOCH_EXHAUSTED);
        return;
    }
    durable_floor_ = durable_seq();
    lifecycle = AYNI_LIFECYCLE_FENCED;
    restriction_ = reason;
    maintenance.end();
    budget.cancel_compaction();
    boot.isolate(log.writer_epoch() + 1U);
}

// After each transition and each step in RUNNING: a compaction starts once
// the log asks for one and the journal has stored everything, so the
// snapshot covers every transition applied.
void ayni_kernel::refresh_storage() {
    if (lifecycle != AYNI_LIFECYCLE_RUNNING || maintaining()) {
        return;
    }
    budget.refresh(host);
    if (budget.compaction_due() && log.idle()) {
        begin_maintenance(false);
    }
}

// A runtime snapshot covers everything the log stored; a migration's covers
// what recovery read, as the writer that stored it left it.
void ayni_kernel::begin_maintenance(bool migration) {
    if (maintenance.generation_exhausted()) {
        enter_restricted(AYNI_RESTRICTION_GENERATION_EXHAUSTED);
        return;
    }
    // Without room for the snapshot the log stays; a migration cannot
    // leave its schema 0 log behind, so it cannot go on.
    if (!budget.refresh_for_snapshot(host)) {
        if (migration) {
            enter_restricted(AYNI_RESTRICTION_STORAGE_FULL);
        }
        return;
    }
    ayni::kernel::SnapshotBoundary boundary{};
    if (migration) {
        boundary.covers_commit_seq = boot.durable_seq();
        boundary.max_transition_id = boot.max_transition_id();
        boundary.boot_epoch = boot.last_boot_epoch();
        boundary.writer_epoch = boot.recovered_writer_epoch();
        maintenance.start(boundary, boot.writer_epoch(), true);
    } else {
        boundary.covers_commit_seq = log.durable_seq();
        boundary.max_transition_id = work.transitions.latest().transition_id;
        boundary.boot_epoch = log.boot_epoch();
        boundary.writer_epoch = log.writer_epoch();
        maintenance.start(boundary, log.writer_epoch(), false);
    }
}

// A runtime run that fails fences the writer, as a failed append does. A
// migration that fails restricts: it ran during recovery, whose model it
// discards. A migration that succeeds recovers again, now from the snapshot
// it activated; a runtime one hands the buffer back to the log, which goes
// on after the activation batch.
void ayni_kernel::settle_maintenance() {
    if (!maintaining() || lifecycle == AYNI_LIFECYCLE_DRAINING ||
        lifecycle == AYNI_LIFECYCLE_STOPPED) {
        return;
    }
    const bool migration = maintenance.mode() == ayni::kernel::StorageMaintenance::Mode::migration;
    if (maintenance.failure() != AYNI_RESTRICTION_NONE) {
        const ayni_restriction_t reason = maintenance.failure();
        maintenance.end();
        if (migration) {
            replay.discard();
            enter_restricted(reason);
        } else {
            enter_fenced(reason);
        }
        return;
    }
    if (!maintenance.done()) {
        return;
    }
    maintenance.end();
    if (migration) {
        replay.discard_for_reload();
        boot.reset();
        return;
    }
    // The activation took the next identity, so no transition ran during
    // the run; anything else is a defect.
    const ayni::durability::BatchIdentity activated = maintenance.activation_batch();
    const ayni::foundation::TransitionStamp stamp = work.transitions.next();
    if (stamp.commit_seq != activated.commit_seq ||
        stamp.transition_id != activated.transition_id) {
        enter_restricted(AYNI_RESTRICTION_INTERNAL);
        return;
    }
    maintenance.adopt(maintenance.metadata().generation, maintenance.slot());
    budget.compacted(maintenance.retained_log_bytes());
    log.start(log.boot_epoch(),
              {log.writer_epoch(), activated.commit_seq + 1U, activated.transition_id + 1U},
              batch_max);
}

// C-AUTH-027: release the deny before retiring its pinned command, but only
// after the full accepted identity matches a retained durable result.
bool ayni_kernel::handoff_revocation(const ayni::kernel::PendingRevoke &pending) {
    const ayni::kernel::CommandMapping *matched = nullptr;
    return work.commands.recovered_revoke(pending, matched) ==
               ayni::kernel::CommandTable::Lookup::retried &&
           matched != nullptr && work.grants.release_gate(pending.accepted.target_grant_id) &&
           work.commands.retire_pending(pending.accepted.principal_id, pending.accepted.command_id);
}

// The active writer acknowledges only controls queued in that same boot.
bool ayni_kernel::retire_durable_revocations() {
    while (const auto *pending =
               work.commands.next_durable_pending(log.boot_epoch(), log.durable_seq())) {
        if (!handoff_revocation(*pending)) {
            return false;
        }
    }
    return true;
}

// The complete recovered prefix is durable regardless of the abandoned
// writer's queued stamp. Only the retained full canonical proves identity.
bool ayni_kernel::reconcile_recovered_revocations() {
    for (uint32_t index = 0; index < work.commands.size(); ++index) {
        const auto &mapping = work.commands.at(index);
        const auto *pending = work.commands.pending(mapping.principal_id, mapping.command_id);
        if (pending != nullptr && !handoff_revocation(*pending)) {
            return false;
        }
    }
    return true;
}

// A pending control or cleanup can make progress only with writable space;
// draining accepts controls already promised, but creates no cleanup work.
bool ayni_kernel::revocation_ready() const {
    if (!appends() || !has_room()) {
        return false;
    }
    const auto *pending = work.commands.next_pending_revoke();
    if (pending != nullptr && work.commands.can_publish_accepted(*pending)) {
        return true;
    }
    ayni::authority::GrantTable::CleanupPlan plan{};
    return lifecycle == AYNI_LIFECYCLE_RUNNING && work.grants.next_revocation_cleanup(plan);
}
