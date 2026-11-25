// Kernel lifecycle (contracts/domain/lifecycles.md, ADR-0016): recovery runs
// in bounded steps after ayni_start, entering RUNNING starts the work model
// (ADR-0017), steps take posted events (ADR-0021) and, in RUNNING, do the
// goals' due work (ADR-0018, ADR-0020) and have the strategies decide READY
// tasks (ADR-0022), and shutdown completes in a step once no completion slot
// is in flight (C-HOST-018).
//
// Storage upkeep (ADR-0027) runs in the same steps. A RUNNING kernel whose
// log nears its limit compacts it into a snapshot. StorageMaintenance holds the
// storage buffer and no transition starts until it is done. A writer whose
// retries run out is fenced: FENCED isolates the old writer, then recovery
// runs again on what is durable and RUNNING resumes under a new boot epoch.

#include "catalog/catalog_id.h"
#include "kernel/runtime/kernel_state.h"
#include "kernel/service/decision/decision.h"
#include "kernel/service/intent/authority_intent.h"
#include "kernel/service/memory/privacy_service.h"
#include "kernel/service/model/work_model.h"

namespace {

using ayni::kernel::ItemEnd;

// How each owner operation's result ends its item (ayni_kernel::run_transition).
ItemEnd ended(ayni::kernel::DueWorkResult result) {
    switch (result) {
    case ayni::kernel::DueWorkResult::none:
        return {ItemEnd::Kind::none};
    case ayni::kernel::DueWorkResult::applied:
        return {ItemEnd::Kind::applied};
    case ayni::kernel::DueWorkResult::internal:
        break;
    }
    return {ItemEnd::Kind::defect, AYNI_RESTRICTION_INTERNAL};
}

ItemEnd ended(ayni::kernel::DecisionResult result) {
    switch (result) {
    case ayni::kernel::DecisionResult::none:
        return {ItemEnd::Kind::none};
    case ayni::kernel::DecisionResult::applied:
        return {ItemEnd::Kind::applied};
    case ayni::kernel::DecisionResult::internal:
        break;
    }
    return {ItemEnd::Kind::defect, AYNI_RESTRICTION_INTERNAL};
}

ItemEnd ended(ayni::kernel::RuntimeAdvance result) {
    switch (result) {
    case ayni::kernel::RuntimeAdvance::idle:
        return {ItemEnd::Kind::none};
    case ayni::kernel::RuntimeAdvance::applied:
        return {ItemEnd::Kind::applied};
    case ayni::kernel::RuntimeAdvance::attempt_exhausted:
        return {ItemEnd::Kind::defect, AYNI_RESTRICTION_ATTEMPT_ID_EXHAUSTED};
    case ayni::kernel::RuntimeAdvance::internal:
        break;
    }
    return {ItemEnd::Kind::defect, AYNI_RESTRICTION_INTERNAL};
}

} // namespace

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
    const auto closed = catalogs.close();
    if (closed != AYNI_OK) {
        return closed;
    }
    const auto scenes = validate_execution_scenes();
    if (scenes != AYNI_OK) {
        return scenes;
    }
    const auto reflexes = reflex_table.validate_all(workspace, catalogs, work.agents, work.scenes);
    if (reflexes != AYNI_OK) {
        return reflexes;
    }
    ingress.open();
    lifecycle = AYNI_LIFECYCLE_RECOVERING;
    audit_lifecycle(AYNI_LIFECYCLE_RECOVERING);
    return AYNI_OK;
}

// Catalogs::close checks the execution catalog itself; the Scene a binding
// names is authority's.
ayni_status_t ayni_kernel::validate_execution_scenes() const {
    for (uint32_t at = 0; at < catalogs.execution.binding_count(); ++at) {
        const auto &binding = catalogs.execution.binding(at);
        uint32_t scene = 0;
        if (!work.scenes.find_definition({binding.scene.data(), binding.scene.size()}, scene)) {
            return AYNI_NOT_FOUND;
        }
        if (work.scenes.definition(scene).version != binding.scene_version) {
            return AYNI_UNSUPPORTED_VERSION;
        }
    }
    return AYNI_OK;
}

ayni::work::CalendarClock ayni_kernel::calendar_clock(uint64_t monotonic_us) const {
    ayni_utc_sample_v1 sample{};
    sample.struct_size = sizeof(sample);
    if (host.utc_now != nullptr) {
        host.utc_now(host.ctx, &sample);
    }
    return {sample.utc_s, monotonic_us,
            sample.struct_size == sizeof(sample) && sample.trust == AYNI_UTC_TRUSTED &&
                sample.utc_s >= 0 && sample.utc_s <= AYNI_CALENDAR_SECONDS_MAX};
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
    StepContext progress{now_us(),
                         max_items,
                         max_time_us,
                         {host.ctx, host.monotonic_now_us},
                         observation_ingress.queued()};
    while (progress.available() && completions.apply_posted(1) == 1U) {
        progress.applied();
    }
    settle_recovery();
    settle_log();
    settle_maintenance();
    refresh_storage();
    advance_recovery_and_maintenance(progress);
    apply_revocations(progress);
    apply_observation_gaps(progress);
    apply_realtime_ingress(progress);
    if (!calls.refresh_live_gates(progress.now_us(), log.durable_seq(), call_mode())) {
        enter_restricted(AYNI_RESTRICTION_INTERNAL);
    }
    apply_calls(progress);
    reflex_engine.reap_completed();
    if (!work.calendars.empty()) {
        work.refresh_calendars(calendar_clock(progress.started_us));
    }
    const auto accepted = apply_ready_work(progress);
    if (!calls.refresh_live_gates(progress.now_us(), log.durable_seq(), call_mode())) {
        enter_restricted(AYNI_RESTRICTION_INTERNAL);
    }
    take_queued_inputs(progress);
    take_queued_observations(progress);
    prepare_accepted_calls(progress, accepted);
    if (appends() && log.advance(progress.now_us())) {
        settle_log();
        if (!retire_durable_revocations()) {
            enter_restricted(AYNI_RESTRICTION_INTERNAL);
        }
    }
    if (lifecycle == AYNI_LIFECYCLE_DRAINING && completions.in_flight() == 0U && calls.idle() &&
        (!appends() || log.idle() || log.failure() != AYNI_RESTRICTION_NONE) &&
        !revocation_ready()) {
        events_discarded += ingress.close();
        lifecycle = AYNI_LIFECYCLE_STOPPED;
        audit_lifecycle(AYNI_LIFECYCLE_STOPPED);
    }

    report_step(progress, report);
    return AYNI_OK;
}

// Recovery and maintenance share the storage buffer and each advance at most
// once per call; maintenance starts only after recovery enters RUNNING.
void ayni_kernel::advance_recovery_and_maintenance(StepContext &progress) {
    if ((lifecycle == AYNI_LIFECYCLE_RECOVERING || lifecycle == AYNI_LIFECYCLE_FENCED) &&
        progress.available() && !maintaining() && boot.advance(progress.now_us())) {
        progress.storage_advanced();
        settle_recovery();
    }
    if (lifecycle == AYNI_LIFECYCLE_RUNNING && maintaining() && progress.available() &&
        maintenance.advance(progress.now_us())) {
        progress.storage_advanced();
        settle_maintenance();
    }
}

// C-AUTH-026/027: accepted revocations precede new work, also while draining;
// descendant cleanup only advances while RUNNING. Journal each accepted change.
void ayni_kernel::apply_revocations(StepContext &progress) {
    if (appends()) {
        if (!retire_durable_revocations()) {
            enter_restricted(AYNI_RESTRICTION_INTERNAL);
        }
        while ((lifecycle == AYNI_LIFECYCLE_RUNNING || lifecycle == AYNI_LIFECYCLE_DRAINING) &&
               appends() && progress.available() && has_room() && run_transition([&] {
                   const uint64_t before = work.transitions.next_commit_seq();
                   bool applied = ayni::kernel::apply_pending_revoke(work);
                   if (!applied && lifecycle == AYNI_LIFECYCLE_RUNNING) {
                       applied = ayni::kernel::apply_revocation_cleanup(work);
                   }
                   if (!applied && lifecycle == AYNI_LIFECYCLE_RUNNING) {
                       applied =
                           work.advance_authority_cleanup(progress.now_us(), log.durable_seq());
                   }
                   if (!applied && privacy_ready()) {
                       applied = ayni::kernel::advance_privacy(work, maintenance.generation());
                   }
                   // A transition none of them reports applied is a defect.
                   if (work.transitions.next_commit_seq() == before) {
                       return ItemEnd{ItemEnd::Kind::none};
                   }
                   return applied ? ItemEnd{ItemEnd::Kind::applied}
                                  : ItemEnd{ItemEnd::Kind::defect, AYNI_RESTRICTION_INTERNAL};
               })) {
            progress.applied();
        }
    }
}

ayni::kernel::CallRuntime::Mode ayni_kernel::call_mode() const {
    using Mode = ayni::kernel::CallRuntime::Mode;
    if (lifecycle == AYNI_LIFECYCLE_RUNNING) {
        return Mode::running;
    }
    if (lifecycle == AYNI_LIFECYCLE_DRAINING) {
        // ADR-0016/0050: an unwritable shutdown releases volatile borrows without
        // inventing durable cancellation. A restored medium can journal again.
        return appends() && log.failure() == AYNI_RESTRICTION_NONE && budget.admits()
                   ? Mode::draining
                   : Mode::closing;
    }
    return Mode::fenced;
}

// Run a bounded number of call transitions under the same journal admission as
// decisions. In a fenced writer only nonpersistent Provider cancellation is allowed.
void ayni_kernel::apply_calls(StepContext &progress) {
    using Runtime = ayni::kernel::CallRuntime;
    while (progress.available()) {
        // journal() can fence/restrict this writer. Never carry RUNNING across
        // that transition into another call dispatch in the same bounded step.
        const auto mode = call_mode();
        const auto admission =
            has_room() ? Runtime::JournalAdmission::available : Runtime::JournalAdmission::blocked;
        if (!run_transition([&] {
                return ended(
                    calls.advance_managed(progress.now_us(), log.durable_seq(), mode, admission));
            })) {
            break;
        }
        progress.applied();
    }
}

// Preserve one schedule/READY frontier for this call so newly derived tasks
// wait until the next decision pass, independent of a strategy's answers.
ayni::kernel::CallRuntime::AcceptedCalls ayni_kernel::apply_ready_work(StepContext &progress) {
    // Due work is taken as of the start of the step, so one step sees one
    // schedule. It comes before events: a Host can keep the queue full, but
    // the due work at one time is finite, so neither starves the other.
    // Decisions likewise take only the tasks READY before the due work, so a
    // task derived in this step is decided in the next and a step does
    // finite work however the strategies answer. A transition that
    // journal() restricts on ends both loops: nothing after it is stored.
    ayni::kernel::CallRuntime::AcceptedCalls accepted;
    if (lifecycle == AYNI_LIFECYCLE_RUNNING) {
        const uint64_t ready_before = work.transitions.next_commit_seq();
        const uint64_t volatile_before = work.tasks.next_volatile_generation();
        while (lifecycle == AYNI_LIFECYCLE_RUNNING && progress.available() && has_room() &&
               run_transition([&] {
                   return ended(ayni::kernel::do_due_work(work, progress.started_us,
                                                          progress.now_us(), log.durable_seq()));
               })) {
            progress.applied();
        }
        uint64_t call_id = 0;
        while (lifecycle == AYNI_LIFECYCLE_RUNNING && progress.available() && run_transition([&] {
                   return ended(ayni::kernel::decide_next(work, decisions, ready_before,
                                                          progress.started_us, log.durable_seq(),
                                                          {host.ctx, host.monotonic_now_us}, calls,
                                                          call_id, volatile_before, has_room()));
               })) {
            accepted.include(call_id);
            progress.applied();
        }
    }
    return accepted;
}

void ayni_kernel::prepare_accepted_calls(StepContext &progress,
                                         const ayni::kernel::CallRuntime::AcceptedCalls &accepted) {
    // Append the available prefix immediately if optional preparation cannot fit or yield work.
    // RAM-only EPHEMERAL calls consume no WAL bytes and may prepare even when the log is idle.
    while (accepted.first && lifecycle == AYNI_LIFECYCLE_RUNNING && progress.available() &&
           completions.posted_count() == 0 && !revocation_ready() &&
           !work.authority_cleanup.ready() && !privacy_ready()) {
        using Admission = ayni::kernel::CallRuntime::JournalAdmission;
        const bool room = has_room();
        const auto admission = room ? Admission::available : Admission::blocked;
        const bool ephemeral_only = !(room && log.can_extend_group(progress.now_us()));
        if (!run_transition([&] {
                return ended(calls.prepare_accepted(accepted, progress.cutoff_us(),
                                                    log.durable_seq(), ephemeral_only, admission));
            })) {
            break;
        }
        progress.applied();
    }
}

// Consume a bounded snapshot of the queue, preserving work-before-input order.
void ayni_kernel::take_queued_inputs(StepContext &progress) {
    // Only the events already queued are taken; later ones wait for the next
    // step. An event's occurrences fall due at the step's start, and the
    // step reports the work they make due as ready.
    if (takes_events()) {
        uint32_t waiting = ingress.queued();
        ayni::catalog::CatalogId source;
        ayni::work::InputResumeBoundary input{};
        while (waiting > 0U && progress.available() && takes_event_now() &&
               ingress.take_local_input(source, &input)) {
            --waiting;
            handle_local_input(source, progress.started_us, input);
            progress.applied();
        }
    }
}

// Gap overlays are source-bounded refusal bookkeeping, like the existing
// scheduling-frontier scans, rather than application work items. Visit each
// source once so repeated producer gaps cannot starve ordinary due work.
void ayni_kernel::apply_observation_gaps(StepContext &) {
    if (lifecycle != AYNI_LIFECYCLE_RUNNING) {
        return;
    }
    for (uint32_t source = 0; source < catalogs.observation_sources.size(); ++source) {
        ayni::knowledge::ObservationBoundary boundary{};
        if (observation_ingress.take_gap(source, boundary)) {
            observations.invalidate(source, boundary);
        }
    }
    for (uint32_t index = 0; index < observation_ingress.subscription_capacity(); ++index) {
        ayni_subscription_ref_v1 reference{};
        ayni::knowledge::ObservationBoundary boundary{};
        if (observation_ingress.take_subscription_gap(index, reference, boundary)) {
            observations.invalidate_subscription(index, reference.generation, boundary);
        }
    }
}

// When a short step is spent on samples, evaluate those samples before reading
// more. Give the ordinary Call lane a turn if the reflex pass also fills a step.
void ayni_kernel::apply_realtime_ingress(StepContext &progress) {
    if (lifecycle != AYNI_LIFECYCLE_RUNNING) {
        realtime_turn_ = RealtimeTurn::samples;
        reflex_engine.reset_pass();
        return;
    }
    if (!progress.available()) {
        return;
    }
    if (realtime_turn_ == RealtimeTurn::calls) {
        realtime_turn_ = RealtimeTurn::samples;
        return;
    }
    if (realtime_turn_ == RealtimeTurn::samples && !reflex_engine.pending()) {
        drain_observation_slots(progress);
        if (!progress.available()) {
            realtime_turn_ = reflex_table.count() ? RealtimeTurn::reflexes : RealtimeTurn::calls;
            return;
        }
    }
    evaluate_reflexes(progress);
    realtime_turn_ = progress.available() ? RealtimeTurn::samples : RealtimeTurn::calls;
}

// Drains each source at most once per pass; the cursor prevents a busy source
// monopolizing shorter steps. Even a dropped old-boot sample spends an item.
void ayni_kernel::drain_observation_slots(StepContext &progress) {
    const uint32_t count = catalogs.observation_sources.size();
    using Drain = ayni::kernel::ObservationSlots::Drain;
    ayni::knowledge::ObservationInput input{};
    for (uint32_t scanned = 0; scanned < count && progress.available(); ++scanned) {
        const uint32_t source = next_observation_slot_;
        next_observation_slot_ = (source + 1U) % count;
        const auto drained = observation_slots.drain(source, input);
        if (drained == Drain::empty) {
            continue;
        }
        if (drained == Drain::stale) {
            progress.applied();
            continue;
        }
        const bool owner_known =
            !input.media || work.agents.find_instance(input.media->owner_instance_id) != nullptr;
        const ayni_status_t status =
            owner_known ? observations.publish_from_ingress(input, source, progress.now_us())
                        : AYNI_NOT_FOUND;
        if (status == AYNI_OK) {
            observation_slots.record_applied();
        } else {
            observation_slots.record_discarded();
        }
        progress.applied();
    }
}

// Evaluates local deterministic ReflexRules right after draining sensor slots and before
// gate refresh / urgent executor stops (0 LLM, 0 WAL, ADR-0112).
void ayni_kernel::evaluate_reflexes(StepContext &progress) {
    using Advance = ayni::kernel::ReflexEngine::Advance;
    while (progress.available()) {
        const auto result = reflex_engine.advance({host.ctx, host.monotonic_now_us},
                                                  progress.cutoff_us(), log.durable_seq());
        if (result == Advance::internal) {
            enter_restricted(AYNI_RESTRICTION_INTERNAL);
        }
        if (result == Advance::timed_out) {
            progress.check_time();
        }
        if (result != Advance::applied) {
            break;
        }
        progress.applied();
        if (!reflex_engine.pending()) {
            break;
        }
    }
}
// Accepted samples consume the same bounded item/time budget after ordinary
// work and inputs. Expiry/future checks use owner application time, not arrival TTL.
void ayni_kernel::take_queued_observations(StepContext &progress) {
    auto waiting = progress.observations_ready;
    ayni::kernel::QueuedObservation next{};
    while (lifecycle == AYNI_LIFECYCLE_RUNNING && waiting != 0 && progress.available() &&
           observation_ingress.take(next)) {
        --waiting;
        const bool owner_known =
            !next.input.media ||
            work.agents.find_instance(next.input.media->owner_instance_id) != nullptr;
        ayni_status_t status = AYNI_NOT_FOUND;
        if (owner_known && next.scope) {
            const auto &scope = *next.scope;
            const bool current =
                scope.reference.boot_epoch == work.ids.boot_epoch() &&
                work.calls.occupied(scope.call_index) &&
                work.calls.identity(scope.call_index).call_id == scope.reference.subscription_id &&
                work.calls.state(scope.call_index).subscription &&
                work.calls.state(scope.call_index).subscription->generation ==
                    scope.reference.generation &&
                calls.subscription_usable(scope.call_index, progress.now_us(), log.durable_seq());
            status = current ? observations.publish_subscription(next.input, scope.call_index,
                                                                 scope.reference.subscription_id,
                                                                 progress.now_us())
                             : AYNI_STALE;
        } else if (owner_known) {
            status =
                observations.publish_from_ingress(next.input, next.source_index, progress.now_us());
        }
        if (status != AYNI_OK) {
            observation_ingress.discarded();
        }
        progress.applied();
    }
}

// Report all readiness even when this call spent its budget: a false idle
// would lose the owner's next wakeup. Slot and storage backpressure still apply.
void ayni_kernel::report_step(StepContext &progress, ayni_step_report_v1 &report) {
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
    // Durable work waits for storage capacity. Eligible RAM-only decisions
    // still run, and report readiness through the same predicate.
    progress.next_deadline_us = ayni::work::no_time;
    const bool room = has_room();
    const bool work_due =
        lifecycle == AYNI_LIFECYCLE_RUNNING &&
        ((room && ayni::kernel::refresh_due_work(work, progress.started_us,
                                                 progress.next_deadline_us, log.durable_seq())) ||
         ayni::kernel::awaits_decision(work, log.durable_seq(), room));
    const bool maintenance_due = maintaining() && maintenance.wants_advance(progress.started_us) &&
                                 completions.has_free_slot();
    // A request held back by the write interval or a retry backoff is due
    // later; one due already is in more_ready instead.
    const uint64_t storage_due_us = storage_deadline();
    progress.include_deadline(storage_due_us);
    const auto mode = call_mode();
    using Admission = ayni::kernel::CallRuntime::JournalAdmission;
    const auto admission = room ? Admission::available : Admission::blocked;
    const bool call_ready = calls.control_ready(progress.started_us, log.durable_seq(), mode) ||
                            calls.ready(progress.started_us, log.durable_seq(), mode, admission);
    const auto call_deadline = calls.next_deadline_us(progress.started_us, admission);
    progress.include_deadline(call_deadline);
    const uint64_t reflex_deadline =
        lifecycle == AYNI_LIFECYCLE_RUNNING
            ? reflex_engine.next_deadline_us(progress.started_us, log.durable_seq())
            : ayni::work::no_time;
    progress.include_deadline(reflex_deadline);
    const bool reflex_due =
        lifecycle == AYNI_LIFECYCLE_RUNNING &&
        (realtime_turn_ == RealtimeTurn::reflexes || reflex_engine.pending() ||
         (reflex_deadline != ayni::work::no_time && reflex_deadline <= progress.started_us));
    report = ayni_step_report_v1{};
    report.struct_size = sizeof(report);
    report.items_done = progress.items_done();
    const bool events_due = takes_events() && takes_event_now() && ingress.queued() > 0U;
    report.more_ready =
        completions.posted_count() > 0U || boot_due || log_due || maintenance_due || events_due ||
                work_due || call_ready || revocation_ready() || reflex_due ||
                (lifecycle == AYNI_LIFECYCLE_RUNNING &&
                 (observation_ingress.queued() != 0 ||
                  observation_ingress.info().gaps_pending != 0 || observation_slots.any_dirty())) ||
                (lifecycle == AYNI_LIFECYCLE_RUNNING && has_room() &&
                 (work.authority_cleanup.ready() || privacy_ready()))
            ? 1U
            : 0U;
    // Re-read civil time within one second even without storage/events: OS clock
    // correction must not leave an old projection asleep indefinitely (ADR-0061).
    constexpr uint64_t calendar_poll_us = 1000000;
    if (lifecycle == AYNI_LIFECYCLE_RUNNING) {
        for (uint32_t index = 0; index < work.goals.size(); ++index) {
            if (ayni::work::is_calendar_trigger(work.goals.at(index).trigger.kind) &&
                ayni::work::GoalTable::is_live(work.goals.at(index).lifecycle)) {
                progress.include_deadline(
                    ayni::work::time_after(progress.started_us, calendar_poll_us));
                break;
            }
        }
    }
    report.next_deadline_us = progress.next_deadline_us;
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
        observation_ingress.close();
        observation_slots.close();
        lifecycle = AYNI_LIFECYCLE_DRAINING;
        audit_lifecycle(AYNI_LIFECYCLE_DRAINING);
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
    work.request_authority_cleanup(stamp);
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

void ayni_kernel::audit_lifecycle(ayni_lifecycle_t next, ayni_restriction_t reason) {
    constexpr char label[] = "kernel";
    ayni::kernel::AuditEntry entry{};
    entry.kind = AYNI_AUDIT_LIFECYCLE;
    entry.boot_epoch = boot.boot_epoch();
    entry.at_us = now_us();
    entry.status = reason == AYNI_RESTRICTION_NONE ? AYNI_OK : AYNI_INVALID_STATE;
    entry.reason = static_cast<ayni_reason_t>(reason);
    entry.detail_code = next;
    entry.label = {label, static_cast<uint32_t>(sizeof(label) - 1U)};
    audit_ring.emit(entry);
}

void ayni_kernel::enter_restricted(ayni_restriction_t reason) {
    observation_ingress.close();
    observation_slots.close();
    lifecycle = AYNI_LIFECYCLE_RESTRICTED;
    restriction_ = reason;
    audit_lifecycle(AYNI_LIFECYCLE_RESTRICTED, reason);
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

void ayni_kernel::handle_local_input(const ayni::catalog::CatalogId &source, uint64_t now_us,
                                     ayni::work::InputResumeBoundary input) {
    if (lifecycle != AYNI_LIFECYCLE_RUNNING) {
        ++events_discarded;
        return;
    }
    static_cast<void>(run_transition([&] {
        if (!ayni::kernel::apply_local_input(work, source, now_us, input)) {
            ++events_unmatched;
            return ItemEnd{ItemEnd::Kind::none};
        }
        return ItemEnd{ItemEnd::Kind::applied};
    }));
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
        if (completions.in_flight() == 0U && calls.ephemeral_idle()) {
            const uint32_t epoch = boot.writer_epoch();
            replay.discard_for_reload();
            log_started_ = false;
            boot.reset(epoch);
            lifecycle = AYNI_LIFECYCLE_RECOVERING;
        }
        break;
    case ayni::kernel::BootSequence::Outcome::pending:
        break;
    }
}

// Transitions follow the boot batch, and the work model starts before any
// intent is accepted. The log takes the storage buffer over from recovery.
void ayni_kernel::enter_running() {
    if (!replay.complete() || !replay.consistent(ayni::kernel::WorkReplay::unbounded)) {
        replay.discard();
        enter_restricted(AYNI_RESTRICTION_LOG_CORRUPT);
        return;
    }
    if (!calls.rebind_restored()) {
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
    ingress.establish_incarnation(boot.boot_epoch());
    lifecycle = AYNI_LIFECYCLE_RUNNING;
    audit_lifecycle(AYNI_LIFECYCLE_RUNNING);
    if (!reconcile_recovered_revocations()) {
        enter_restricted(AYNI_RESTRICTION_LOG_CORRUPT);
        return;
    }
    observations.begin_boot(boot.boot_epoch());
    observation_ingress.begin_boot(boot.boot_epoch());
    observation_slots.begin_boot(boot.boot_epoch());
    reflex_table.begin_boot();
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
    observation_ingress.close();
    observation_slots.close();
    lifecycle = AYNI_LIFECYCLE_FENCED;
    restriction_ = reason;
    audit_lifecycle(AYNI_LIFECYCLE_FENCED, reason);
    maintenance.end();
    budget.cancel_compaction();
    boot.isolate(log.writer_epoch() + 1U);
}

// After each transition and each step in RUNNING: a compaction starts once
// the log asks for one and the journal has stored everything, so the
// snapshot covers every transition applied.
// Refresh the admission owner during shutdown too: a cached full-medium refusal
// must not hold pre-dispatch borrows forever after the Host restores capacity.
// DRAINING never starts a new compaction; it only drains promised work (C-LIFE-002, ADR-0027).
void ayni_kernel::refresh_storage() {
    if ((lifecycle != AYNI_LIFECYCLE_RUNNING && lifecycle != AYNI_LIFECYCLE_DRAINING) ||
        maintaining()) {
        return;
    }
    budget.refresh(host);
    if (lifecycle == AYNI_LIFECYCLE_DRAINING) {
        // Keep the logical log limit: when compaction would be required,
        // closing releases only volatile borrows instead of growing the log.
        return;
    }
    const auto &privacy = work.privacy.record();
    const bool privacy_rotation = privacy.phase == ayni::work::PrivacyPhase::compact &&
                                  maintenance.generation() < privacy.snapshot_generation;
    if ((budget.compaction_due() || privacy_rotation) && log.idle()) {
        begin_maintenance();
    }
}

bool ayni_kernel::privacy_ready() const {
    if (lifecycle != AYNI_LIFECYCLE_RUNNING || !work.privacy.pending() ||
        work.privacy.record().stamp.commit_seq > log.durable_seq()) {
        return false;
    }
    if (work.privacy.record().phase == ayni::work::PrivacyPhase::files &&
        (!calls.privacy_released() || !work.contexts.files_current())) {
        return false;
    }
    ayni::work::PrivacyRecord next{};
    return ayni::kernel::next_privacy_step(work, maintenance.generation(), next);
}

// Compaction covers the current writer's complete durable work boundary.
void ayni_kernel::begin_maintenance() {
    if (maintenance.generation_exhausted()) {
        enter_restricted(AYNI_RESTRICTION_GENERATION_EXHAUSTED);
        return;
    }
    // Without room for a candidate the current durable log remains authoritative.
    if (!budget.refresh_for_snapshot(host)) {
        return;
    }
    const ayni::kernel::SnapshotBoundary boundary{log.durable_seq(),
                                                  work.transitions.latest().transition_id,
                                                  log.boot_epoch(), log.writer_epoch()};
    maintenance.start(boundary, log.writer_epoch());
}

// Failed compaction fences the writer as append failure does. Success returns
// the buffer to the log, which continues after its durable activation batch.
void ayni_kernel::settle_maintenance() {
    if (!maintaining() || lifecycle == AYNI_LIFECYCLE_DRAINING ||
        lifecycle == AYNI_LIFECYCLE_STOPPED) {
        return;
    }
    if (maintenance.failure() != AYNI_RESTRICTION_NONE) {
        const ayni_restriction_t reason = maintenance.failure();
        maintenance.end();
        enter_fenced(reason);
        return;
    }
    if (!maintenance.done()) {
        return;
    }
    maintenance.end();
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
