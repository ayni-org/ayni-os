#include "experiment_composite.h"
namespace ayni::runtime::experiments {
namespace {
struct Boundary {
    RecordPoint record;
    const char *step;
};
Boundary boundary(CompositeCut cut) {
    switch (cut) {
    case CompositeCut::action_receipt:
        return {RecordPoint::terminal_call, "increment.v1"};
    case CompositeCut::capture_prepare:
        return {RecordPoint::prepared_call, "capture.v1"};
    case CompositeCut::capture_dispatch:
        return {RecordPoint::dispatch_call, "capture.v1"};
    case CompositeCut::analysis_prepare:
        return {RecordPoint::prepared_call, "analysis.v1"};
    case CompositeCut::analysis_dispatch:
        return {RecordPoint::dispatch_call, "analysis.v1"};
    case CompositeCut::analysis_receipt:
        return {RecordPoint::terminal_call, "analysis.v1"};
    case CompositeCut::summary:
        return {RecordPoint::memory, nullptr};
    case CompositeCut::capture_stop:
        return {RecordPoint::stopped_subscription, "capture.v1"};
    case CompositeCut::presentation_prepare:
        return {RecordPoint::prepared_call, "present.v1"};
    case CompositeCut::presentation_receipt:
        return {RecordPoint::terminal_call, "present.v1"};
    case CompositeCut::task_complete:
        return {RecordPoint::task_state, nullptr};
    }
    return {RecordPoint::before_marker, nullptr};
}
} // namespace
// Each Case owns one World and one actual accepted append. Stored/lost bracket
// the complete atomic transition; the existing exact-prefix probe handles tears.
bool Composite::restart_boundary(CompositeCut cut, hosts::sim::CutAppend disposition) {
    auto &e = world_.evidence;
    world_.items_per_step = 1;
    e.fact("cut", "composite_boundary", static_cast<uint64_t>(cut));
    const auto target = boundary(cut);
    switch (cut) {
    case CompositeCut::action_receipt:
    case CompositeCut::capture_prepare:
    case CompositeCut::capture_dispatch:
    case CompositeCut::analysis_prepare:
    case CompositeCut::analysis_dispatch:
        if (!advance_action()) {
            return false;
        }
        break;
    case CompositeCut::analysis_receipt:
    case CompositeCut::summary:
    case CompositeCut::capture_stop:
    case CompositeCut::presentation_prepare:
        if (!analysis_pending()) {
            return false;
        }
        model_delivery_ = true;
        break;
    case CompositeCut::presentation_receipt:
    case CompositeCut::task_complete:
        if (!presentation_pending() ||
            !e.status("present before result cut", notification_.execute(world_.node.kernel())) ||
            !e.status("present receipt before cut",
                      notification_.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE))) {
            return false;
        }
        if (cut == CompositeCut::task_complete &&
            !world_.until("completed Task staged for commit",
                          [&] { return task().lifecycle == AYNI_TASK_COMPLETED; })) {
            return false;
        }
        break;
    }
    if (!world_.hold_record_append(target.record, target.step)) {
        return false;
    }
    const auto previous = task();
    const auto action = call("increment.v1"), analysis = call("analysis.v1"),
               presentation = call("present.v1");
    const auto durable_before = world_.info().durable_seq;
    const auto cut_seq = world_.node.simulation()->pending_append()->commit_seq;
    const auto presentations_before = notification_.executions();
    if (!e.equal("first action completed before cut", world_.service.executions(), 1) ||
        !world_.crash(disposition)) {
        return false;
    }
    strategy_version_ = future_version;
    model_delivery_ = false;
    if (!world_.start()) {
        return false;
    }
    if (!e.equal("cut retains original Task", task().task_id, previous.task_id) ||
        !e.equal("cut cannot repeat completed effect", world_.service.executions(), 1)) {
        return false;
    }
    for (const char *key : {"increment.v1", "capture.v1", "analysis.v1", "present.v1"}) {
        const auto current = call(key);
        if ((current.phase == AYNI_CALL_DISPATCHING || current.phase == AYNI_CALL_IN_PROGRESS) &&
            current.attempt.boot_epoch != world_.info().boot_epoch) {
            if (!world_.until("old possible dispatch classified durably", [&] {
                    const auto c = call(key);
                    return c.phase == AYNI_CALL_RESULT_UNKNOWN &&
                           c.requires_seq <= world_.info().durable_seq;
                })) {
                return false;
            }
        }
    }
    // Only durable accepted calls survive a lost append. A stored append also
    // retains its new identity; a lost never-accepted call may be planned anew.
    for (const auto &saved : {action, analysis, presentation}) {
        if (!saved.logical_call_id || !saved.requires_seq ||
            (saved.requires_seq > durable_before &&
             !(disposition == hosts::sim::CutAppend::stored && saved.requires_seq == cut_seq))) {
            continue;
        }
        auto restored = initialized<ayni_call_info_v1>();
        if (!e.status("durable original call retained",
                      ayni_get_call(world_.node.kernel(), saved.logical_call_id, &restored)) ||
            !e.equal("durable operation identity retained", restored.operation_id,
                     saved.operation_id)) {
            return false;
        }
    }
    auto reconcile = [&](const char *key, ActionService &service) {
        const auto current = call(key);
        const ayni_call_query_result_v1 request{sizeof(request), 0, current.logical_call_id};
        world_.submit(AYNI_INTENT_CALL_QUERY_RESULT, &request, 0, current.query_revision);
        return world_.until("original operation query submitted",
                            [&] { return service.pending_queries() == 1; }) &&
               e.status("original external effect found",
                        service.answer_query(AYNI_QUERY_RESULT_FOUND)) &&
               world_.until("original outcome reconciled durably", [&] {
                   const auto c = call(key);
                   return c.query_conclusion == AYNI_QUERY_RESULT_FOUND &&
                          c.requires_seq <= world_.info().durable_seq;
               });
    };
    if (call("increment.v1").phase == AYNI_CALL_RESULT_UNKNOWN &&
        !reconcile("increment.v1", world_.service)) {
        return false;
    }
    const auto restored_analysis = call("analysis.v1"), restored_capture = call("capture.v1");
    if (restored_analysis.phase == AYNI_CALL_RESULT_UNKNOWN ||
        restored_capture.phase == AYNI_CALL_RESULT_UNKNOWN) {
        // Abruptly lost collection has no stop proof; this Mock offers no query
        // resolver. Already accepted historical analysis may finish, but its
        // result cannot create a new candidate or notification after that loss.
        model_delivery_ = restored_analysis.phase != AYNI_CALL_RESULT_UNKNOWN;
        e.fact("recovery", "blocked_capture_unknown",
               restored_capture.phase == AYNI_CALL_RESULT_UNKNOWN);
        e.fact("recovery", "blocked_analysis_unknown",
               restored_analysis.phase == AYNI_CALL_RESULT_UNKNOWN);
        if (!world_.settle(64)) {
            return false;
        }
        const auto expected_candidates =
            (cut == CompositeCut::summary && disposition == hosts::sim::CutAppend::stored) ||
                    cut >= CompositeCut::capture_stop
                ? 1U
                : 0U;
        if (!e.equal("unconfirmed collection/analysis prevents notification",
                     notification_.submissions(), 0) ||
            !e.equal("unconfirmed dependency cannot complete workflow",
                     task().lifecycle == AYNI_TASK_COMPLETED, 0) ||
            !e.equal("no new candidate after unconfirmed dependency", candidates(),
                     expected_candidates) ||
            !e.equal("unconfirmed analysis cannot increase model count",
                     task().model_calls_used <= 1, 1)) {
            return false;
        }
        if (restored_analysis.phase == AYNI_CALL_RESULT_UNKNOWN) {
            return e.equal("unknown analysis consumes once", task().model_calls_used, 1) &&
                   e.equal("unknown analysis keeps original logical call",
                           restored_analysis.logical_call_id, analysis.logical_call_id) &&
                   e.equal("unknown analysis is never rebound", model_.pending(), 0);
        }
        return e.equal("unconfirmed capture retains original identity",
                       call("capture.v1").logical_call_id, restored_capture.logical_call_id);
    }
    if (call("present.v1").phase == AYNI_CALL_RESULT_UNKNOWN &&
        !reconcile("present.v1", notification_)) {
        return false;
    }
    model_delivery_ = true;
    if (!world_.until("recovered work reaches presentation or completion", [&] {
            return notification_.pending() == 1 || task().lifecycle == AYNI_TASK_COMPLETED;
        })) {
        return false;
    }
    if (notification_.pending()) {
        if (!e.equal("only an unperformed presentation may execute", presentations_before, 0) ||
            !finish()) {
            return false;
        }
    } else if (!world_.until("recovered completion durable",
                             [&] { return task().commit_seq <= world_.info().durable_seq; })) {
        return false;
    }
    return e.equal("recovery preserves one completed action", world_.service.executions(), 1) &&
           e.equal("recovery preserves one presentation", notification_.executions(), 1) &&
           e.equal("recovery preserves one model count", task().model_calls_used, 1) &&
           e.equal("atomic summary step creates one candidate", candidates(), 1);
}
bool Composite::run_cut(CompositeCut cut, hosts::sim::CutAppend disposition) {
    completed_ = prepare() && restart_boundary(cut, disposition);
    return completed_;
}
} // namespace ayni::runtime::experiments
