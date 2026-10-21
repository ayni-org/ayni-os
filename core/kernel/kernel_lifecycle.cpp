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
// boot sequence then issues at most one storage request, also one item. The
// request is never awaited: its result arrives in a later step. In RUNNING
// each piece of the goals' due work is then one item, then each decision,
// and after them each event queued when the step comes to the events, again
// with the clock read after each; each of them is one transition, which
// needs room for its batch first (ADR-0026). Last, the log issues the append
// of the oldest batch waiting, whatever is left of the budget: it is one
// call that does not wait, and holding it back would only delay durability.
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
    settle_log();
    settle_maintenance();
    refresh_storage();
    if ((lifecycle == AYNI_LIFECYCLE_RECOVERING || lifecycle == AYNI_LIFECYCLE_FENCED) &&
        items < max_items && !out_of_time && maintenance_mode_ == MaintenanceMode::none &&
        boot.advance(host.monotonic_now_us(host.ctx))) {
        ++items;
        settle_recovery();
    }
    if ((lifecycle == AYNI_LIFECYCLE_RECOVERING || lifecycle == AYNI_LIFECYCLE_RUNNING) &&
        maintenance_mode_ != MaintenanceMode::none && items < max_items && !out_of_time &&
        maintenance.advance(host.monotonic_now_us(host.ctx))) {
        ++items;
        settle_maintenance();
    }
    // Due work is taken as of the start of the step, so one step sees one
    // schedule. It comes before events: a Host can keep the queue full, but
    // the due work at one time is finite, so neither starves the other.
    // Decisions likewise take only the tasks READY before the due work, so a
    // task derived in this step is decided in the next and a step does
    // finite work however the strategies answer. A transition that
    // journal() restricts on ends both loops: nothing after it is stored.
    if (lifecycle == AYNI_LIFECYCLE_RUNNING) {
        const uint64_t ready_before = work.transitions.next_commit_seq();
        while (lifecycle == AYNI_LIFECYCLE_RUNNING && items < max_items && !out_of_time &&
               has_room()) {
            const uint64_t first_seq = work.transitions.next_commit_seq();
            if (!ayni::kernel::do_due_work(work, started_us)) {
                break;
            }
            journal(first_seq);
            ++items;
            out_of_time = time_is_up(host, started_us, max_time_us);
        }
        while (lifecycle == AYNI_LIFECYCLE_RUNNING && items < max_items && !out_of_time &&
               has_room()) {
            const uint64_t first_seq = work.transitions.next_commit_seq();
            if (!ayni::kernel::decide_next(work, decisions, ready_before, started_us)) {
                break;
            }
            journal(first_seq);
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
        while (waiting > 0U && items < max_items && !out_of_time && takes_event_now() &&
               ingress.take_local_input(source)) {
            --waiting;
            ++items;
            handle_local_input(source, started_us);
            out_of_time = time_is_up(host, started_us, max_time_us);
        }
    }
    if (appends() && log.advance(host.monotonic_now_us(host.ctx))) {
        settle_log();
    }
    if (lifecycle == AYNI_LIFECYCLE_DRAINING && completions.in_flight() == 0U &&
        (!appends() || log.idle() || log.failure() != AYNI_RESTRICTION_NONE)) {
        events_discarded += ingress.close();
        lifecycle = AYNI_LIFECYCLE_STOPPED;
    }

    // A request waiting for a slot is not ready: the post that frees one
    // wakes the owner.
    const bool boot_due =
        (lifecycle == AYNI_LIFECYCLE_RECOVERING || lifecycle == AYNI_LIFECYCLE_FENCED) &&
        maintenance_mode_ == MaintenanceMode::none && boot.wants_advance(started_us) &&
        completions.has_free_slot();
    const bool log_due = appends() && log.wants_advance(started_us) && completions.has_free_slot();
    // DRAINING and RESTRICTED do no due work and decide nothing (ADR-0018).
    // Runs after the time budget is spent too: more_ready must not drop due
    // work. A task still READY waits for a decision, also one that a
    // decision reporting progress left READY for a later step (ADR-0024).
    // Without room, all of it waits for an append's post, which wakes the
    // owner.
    uint64_t next_deadline_us = ayni::work::no_time;
    const bool work_due = lifecycle == AYNI_LIFECYCLE_RUNNING && has_room() &&
                          (ayni::kernel::refresh_due_work(work, started_us, next_deadline_us) ||
                           ayni::kernel::awaits_decision(work));
    const bool maintenance_due = maintenance_mode_ != MaintenanceMode::none &&
                                 maintenance.wants_advance(started_us) &&
                                 completions.has_free_slot();
    uint64_t storage_deadline = ayni::work::no_time;
    if (lifecycle == AYNI_LIFECYCLE_RECOVERING || lifecycle == AYNI_LIFECYCLE_FENCED) {
        storage_deadline = maintenance_mode_ == MaintenanceMode::none ? boot.next_deadline()
                                                                      : maintenance.next_deadline();
    } else if (lifecycle == AYNI_LIFECYCLE_RUNNING) {
        storage_deadline = maintenance_mode_ == MaintenanceMode::none ? log.next_deadline()
                                                                      : maintenance.next_deadline();
    } else if (lifecycle == AYNI_LIFECYCLE_DRAINING && appends()) {
        storage_deadline = log.next_deadline();
    }
    if (storage_deadline > started_us && storage_deadline < next_deadline_us) {
        next_deadline_us = storage_deadline;
    }
    report = ayni_step_report_v1{};
    report.struct_size = sizeof(report);
    report.items_done = items;
    const bool events_due = takes_events() && takes_event_now() && ingress.queued() > 0U;
    report.more_ready = completions.posted_count() > 0U || boot_due || log_due || maintenance_due ||
                                events_due || work_due
                            ? 1U
                            : 0U;
    report.next_deadline_us = next_deadline_us;
    report.durable_seq = durable_seq();
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

uint64_t ayni_kernel::durable_seq() const {
    const uint64_t known = log_started_ ? log.durable_seq() : boot.durable_seq();
    return known > durable_floor_ ? known : durable_floor_;
}

void ayni_kernel::journal(uint64_t first_seq) {
    const uint64_t next_seq = work.transitions.next_commit_seq();
    if (next_seq == first_seq || lifecycle != AYNI_LIFECYCLE_RUNNING) {
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
    log_bytes_ += size;
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
// DRAINING and RESTRICTED make no new work (ADR-0018), so they discard them.
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
        if (lifecycle == AYNI_LIFECYCLE_RECOVERING) {
            replay.discard();
        }
        enter_restricted(boot.restriction());
        break;
    case ayni::kernel::BootSequence::Outcome::writers_isolated:
        if (completions.in_flight() == 0U) {
            const uint32_t epoch = boot.writer_epoch();
            replay.discard_for_reload();
            log_started_ = false;
            boot.reset(epoch);
            lifecycle = AYNI_LIFECYCLE_RECOVERING;
        }
        break;
    case ayni::kernel::BootSequence::Outcome::migration_ready:
        if (maintenance_mode_ == MaintenanceMode::none) {
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
    writer_epoch_ = boot.boot_batch().writer_epoch;
    boot_epoch_current_ = boot.boot_epoch();
    generation_ = boot.generation();
    active_slot_ = boot.active_slot();
    log_bytes_ = boot.log_bytes();
    maintenance_requested_ = false;
    reloading_ = false;
    restriction_ = AYNI_RESTRICTION_NONE;
    log.start(boot.boot_epoch(), {boot_batch.writer_epoch, first.commit_seq, first.transition_id},
              batch_max);
    log_started_ = true;
    lifecycle = AYNI_LIFECYCLE_RUNNING;
    if (work.start(boot.boot_epoch(), first)) {
        journal(first.commit_seq);
    }
}

void ayni_kernel::enter_fenced(ayni_restriction_t reason) {
    if (reloading_ || writer_epoch_ == UINT32_MAX) {
        enter_restricted(writer_epoch_ == UINT32_MAX ? AYNI_RESTRICTION_WRITER_EPOCH_EXHAUSTED
                                                     : reason);
        return;
    }
    durable_floor_ = durable_seq();
    lifecycle = AYNI_LIFECYCLE_FENCED;
    restriction_ = reason;
    reloading_ = true;
    maintenance_mode_ = MaintenanceMode::none;
    maintenance_requested_ = false;
    boot.isolate(writer_epoch_ + 1U);
}

void ayni_kernel::refresh_storage() {
    if (lifecycle != AYNI_LIFECYCLE_RUNNING || maintenance_mode_ != MaintenanceMode::none) {
        return;
    }
    storage_space_blocked_ = storage_admission_status() != AYNI_OK;
    const uint64_t reserve = log_max_bytes_ > 0U ? journal_buffer_bytes_ + batch_max + 1024U : 0U;
    if (log_bytes_ >= log_max_bytes_ * 3U / 4U || log_bytes_ + reserve >= log_max_bytes_) {
        maintenance_requested_ = true;
    }
    if (maintenance_requested_ && log.idle()) {
        begin_maintenance(false);
    }
}

void ayni_kernel::begin_maintenance(bool migration) {
    if (generation_ == UINT64_MAX) {
        enter_restricted(AYNI_RESTRICTION_GENERATION_EXHAUSTED);
        return;
    }
    uint64_t available = 0;
    const uint64_t reserve =
        snapshot_max_bytes_ +
        ayni::durability::batch_size(ayni::kernel::activation_record_length, 1) + 2U * batch_max;
    if (host.storage_free_bytes(host.ctx, &available) != AYNI_OK || available < reserve) {
        storage_space_blocked_ = true;
        if (migration) {
            enter_restricted(AYNI_RESTRICTION_STORAGE_FULL);
        }
        return;
    }
    const ayni::kernel::SnapshotLayout layout = ayni::kernel::measure_snapshot(work);
    ayni::durability::SnapshotMetadata metadata{};
    metadata.generation = migration ? 1U : generation_ + 1U;
    metadata.covers_commit_seq = migration ? boot.durable_seq() : log.durable_seq();
    metadata.max_transition_id =
        migration ? boot.max_transition_id() : work.transitions.latest().transition_id;
    metadata.boot_epoch = migration ? boot.last_boot_epoch() : boot_epoch_current_;
    metadata.writer_epoch = migration ? boot.recovered_writer_epoch() : writer_epoch_;
    metadata.payload_length = layout.payload_length;
    metadata.record_count = layout.record_count;
    const uint32_t slot = migration || generation_ == 0U ? 0U : 1U - active_slot_;
    maintenance.start(metadata, slot, migration ? boot.writer_epoch() : writer_epoch_, migration);
    maintenance_mode_ = migration ? MaintenanceMode::migration : MaintenanceMode::runtime;
}

void ayni_kernel::settle_maintenance() {
    if (maintenance_mode_ == MaintenanceMode::none || lifecycle == AYNI_LIFECYCLE_DRAINING ||
        lifecycle == AYNI_LIFECYCLE_STOPPED) {
        return;
    }
    const bool migration = maintenance_mode_ == MaintenanceMode::migration;
    if (maintenance.failure() != AYNI_RESTRICTION_NONE) {
        const ayni_restriction_t reason = maintenance.failure();
        maintenance_mode_ = MaintenanceMode::none;
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
    maintenance_mode_ = MaintenanceMode::none;
    if (migration) {
        replay.discard_for_reload();
        boot.reset();
        return;
    }
    const ayni::durability::BatchIdentity activated = maintenance.activation_batch();
    const ayni::foundation::TransitionStamp stamp = work.transitions.next();
    if (stamp.commit_seq != activated.commit_seq ||
        stamp.transition_id != activated.transition_id) {
        enter_restricted(AYNI_RESTRICTION_INTERNAL);
        return;
    }
    generation_ = maintenance.metadata().generation;
    active_slot_ = maintenance.slot();
    log_bytes_ = maintenance.retained_log_bytes();
    maintenance_requested_ = false;
    log.start(boot_epoch_current_,
              {writer_epoch_, activated.commit_seq + 1U, activated.transition_id + 1U}, batch_max);
}

ayni_status_t ayni_kernel::storage_admission_status() const {
    uint64_t available = 0;
    const uint64_t snapshot_reserve =
        snapshot_max_bytes_ +
        ayni::durability::batch_size(ayni::kernel::activation_record_length, 1) + 2U * batch_max;
    const uint64_t journal_reserve = journal_buffer_bytes_ + batch_max + 1024U;
    const uint64_t required =
        snapshot_reserve > journal_reserve ? snapshot_reserve : journal_reserve;
    if (host.storage_free_bytes == nullptr ||
        host.storage_free_bytes(host.ctx, &available) != AYNI_OK || available < required) {
        return AYNI_STORAGE_FAILED;
    }
    return AYNI_OK;
}
