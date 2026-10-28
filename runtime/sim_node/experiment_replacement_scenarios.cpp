#include "experiment_replacement.h"
#include <cstring>
namespace ayni::runtime::experiments {
// Compare externally visible facts rather than retaining any Core pointers.
// Only due times/boot-derived runtime state may differ after process loss.
bool Replacement::preserved() {
    auto &e = world_.evidence;
    const auto &b = *baseline_;
    const auto current = task(workflow_), pending = task(pending_goal_);
    const auto old_action = call(b.action.logical_call_id), model = call(b.model.logical_call_id);
    const auto private_memory = memory();
    TaskProgressEvidence progress;
    CallRouteEvidence route;
    if (!e.status("inspect restored common checkpoint",
                  inspect_task_progress(world_.node.kernel(), current.task_id, progress)) ||
        !e.status("inspect restored original route",
                  inspect_call_route(world_.node.kernel(), model.logical_call_id, route)) ||
        !e.equal("common checkpoint bytes/writer/steps retained", progress == b.progress, 1) ||
        !e.equal("old model exact route retained", route == b.model_route, 1)) {
        return false;
    }
    uint64_t count = world_.count(AYNI_OBJECT_GRANT);
    bool valid =
        e.equal("same Agent A", world_.instance(1), b.instance_a) &&
        e.equal("same Agent B", world_.instance(2), b.instance_b) &&
        e.equal("same Task", current.task_id, b.task.task_id) &&
        e.equal("same Task owner", current.owner_instance_id, b.task.owner_instance_id) &&
        e.equal("same occurrence", current.occurrence, b.task.occurrence) &&
        e.equal("same common steps", current.step_count, b.task.step_count) &&
        e.equal("same pending Task", pending.task_id, b.pending.task_id) &&
        e.equal("model consumption retained", pending.model_calls_used,
                b.pending.model_calls_used) &&
        e.equal("model ceiling retained", pending.model_call_limit, b.pending.model_call_limit) &&
        e.equal("old model operation retained", model.operation_id, b.model.operation_id) &&
        e.equal("old model boot retained", model.attempt.boot_epoch, b.model.attempt.boot_epoch) &&
        e.equal("old model request retained", model.attempt.request_id,
                b.model.attempt.request_id) &&
        e.equal("completed action operation retained", old_action.operation_id,
                b.action.operation_id) &&
        e.equal("completed action outcome retained", old_action.outcome, b.action.outcome) &&
        e.equal("completed action result publication retained", old_action.requires_seq,
                b.action.requires_seq) &&
        e.equal("memory identity retained", private_memory.memory_id, b.memory.memory_id) &&
        e.equal("memory owner retained", private_memory.namespace_id, b.memory.namespace_id) &&
        e.equal("memory version retained", private_memory.revision, b.memory.revision) &&
        e.equal("no new grants", count, b.grants.size());
    for (const auto &old : b.grants) {
        auto now = initialized<ayni_grant_info_v1>();
        valid =
            e.status("recovered Grant", ayni_get_grant(world_.node.kernel(), old.grant_id, &now)) &&
            valid;
        valid = e.equal("same Grant subject", now.subject_instance_id, old.subject_instance_id) &&
                valid;
        valid = e.equal("same Grant revision", now.revision, old.revision) && valid;
        valid = e.equal("same Grant capability",
                        now.capability_version == old.capability_version &&
                            now.capability_id_size == old.capability_id_size &&
                            std::memcmp(now.capability_id, old.capability_id,
                                        old.capability_id_size) == 0,
                        1) &&
                valid;
        valid = e.equal("same Grant lifecycle", now.lifecycle, old.lifecycle) && valid;
    }
    return valid && e.equal("completed action not replayed", world_.service.executions(), 1);
}
bool Replacement::restart(providers::mock::Variant provider, Policy policy) {
    if (!world_.crash(hosts::sim::CutAppend::lost)) {
        return false;
    }
    provider_ = provider;
    selected_ = policy;
    return world_.start() && preserved();
}
bool Replacement::complete_workflow() {
    auto &e = world_.evidence;
    if (!change_goal(workflow_, AYNI_INTENT_GOAL_RESUME)) {
        return false;
    }
    deliver_ = true;
    if (!world_.until("restored workflow completes", [&] {
            auto t = task(workflow_);
            return t.lifecycle == AYNI_TASK_COMPLETED && t.commit_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    const auto old_action = call(baseline_->action.logical_call_id);
    uint32_t bytes = UINT32_MAX;
    return e.status("old completed result remains readable",
                    ayni_copy_call_result(world_.node.kernel(), old_action.logical_call_id, nullptr,
                                          0, &bytes)) &&
           e.equal("old empty counter result", bytes, 0) &&
           e.equal("no replay after strategy consumption", world_.service.executions(), 1) &&
           e.equal("exactly one original counter submission", world_.service.submissions(), 1) &&
           e.equal("resumed Task keeps identity", task(workflow_).task_id,
                   baseline_->task.task_id) &&
           e.equal("resumed Task uses one new model", task(workflow_).model_calls_used, 1);
}
bool Replacement::provider_swap() {
    const auto old_attempt = a_.attempt();
    if (!world_.evidence.equal("old model packet exists", old_attempt.has_value(), 1) ||
        !restart(providers::mock::Variant::b, Policy::rules)) {
        return false;
    }
    const auto old = baseline_->model;
    if (!world_.until("old request becomes durable unknown", [&] {
            const auto value = call(old.logical_call_id);
            return value.phase == AYNI_CALL_RESULT_UNKNOWN &&
                   value.requires_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    const auto goal = create("replacement.model");
    if (!world_.until("new independent model uses B", [&] { return b_.pending() == 1; })) {
        return false;
    }
    auto &e = world_.evidence;
    const auto current_id = b_.current_call();
    const auto before = call(current_id);
    CallRouteEvidence route;
    if (!e.status("inspect independent B route",
                  inspect_call_route(world_.node.kernel(), current_id, route)) ||
        !e.equal("independent call actually bound to B",
                 route.provider == "mock.b.text" && route.endpoint == "mock.b.model", 1)) {
        return false;
    }
    if (!e.equal("new call differs from unknown call", current_id != old.logical_call_id, 1) ||
        !e.equal("old A has no live arena borrow", a_.pending(), 0) ||
        !e.status("A packet rejected by new B target", b_.post_attempt(*old_attempt),
                  AYNI_INVALID_STATE) ||
        !world_.settle(4)) {
        return false;
    }
    const auto after = call(current_id), unchanged = call(old.logical_call_id);
    if (!e.equal("stale response cannot complete B", after.outcome, before.outcome) ||
        !e.equal("stale response cannot complete A", unchanged.phase, AYNI_CALL_RESULT_UNKNOWN) ||
        !e.equal("unknown operation unchanged", unchanged.operation_id, old.operation_id) ||
        !e.equal("unknown logical call unchanged", task(pending_goal_).logical_call_id,
                 old.logical_call_id) ||
        !e.equal("unknown request consumes no extra model count",
                 task(pending_goal_).model_calls_used, 1)) {
        return false;
    }
    deliver_ = true;
    return world_.until("independent B Task completes",
                        [&] { return task(goal).lifecycle == AYNI_TASK_COMPLETED; }) &&
           e.equal("B executed only independent work", b_.executions(), 1) && complete_workflow();
}
bool Replacement::compatible_swap() {
    return restart(providers::mock::Variant::b, Policy::tool_loop) && complete_workflow() &&
           world_.evidence.equal("compatible ToolLoop completed with B", b_.executions(), 1);
}
// Produce a genuine ToolLoop private checkpoint, then remove its declared
// reader or install a version that never promised to read the old writer.
bool Replacement::incompatible(Policy policy) {
    if (!restart(providers::mock::Variant::a, Policy::tool_loop) ||
        !change_goal(workflow_, AYNI_INTENT_GOAL_RESUME) ||
        !world_.until("ToolLoop call prepared", [&] { return a_.pending() == 1; })) {
        return false;
    }
    if (!world_.until("ToolLoop checkpoint durable",
                      [&] { return task(workflow_).commit_seq <= world_.info().durable_seq; })) {
        return false;
    }
    const auto before = task(workflow_);
    const auto call_before = call(before.logical_call_id);
    TaskProgressEvidence progress, held_progress, rollback_progress;
    if (!world_.evidence.status(
            "inspect real ToolLoop checkpoint",
            inspect_task_progress(world_.node.kernel(), before.task_id, progress)) ||
        !world_.evidence.equal("ToolLoop private bytes exist", !progress.state.empty(), 1)) {
        return false;
    }
    if (!world_.crash(hosts::sim::CutAppend::lost)) {
        return false;
    }
    selected_ = policy;
    if (!world_.start()) {
        return false;
    }
    const auto after = task(workflow_);
    auto &e = world_.evidence;
    if (!e.status("inspect incompatible checkpoint",
                  inspect_task_progress(world_.node.kernel(), after.task_id, held_progress)) ||
        !e.equal("incompatible bytes/writer/steps unchanged", progress == held_progress, 1)) {
        return false;
    }
    if (!e.equal("incompatible Task paused", after.lifecycle, AYNI_TASK_PAUSED) ||
        !e.status("incompatible classified status", after.hold_status, AYNI_UNSUPPORTED_VERSION) ||
        !e.equal("incompatible classified reason", after.hold_reason,
                 AYNI_REASON_STRATEGY_CHANGED) ||
        !e.equal("incompatible Task not reset", after.task_id, before.task_id) ||
        !e.equal("incompatible steps not reset", after.step_count, before.step_count) ||
        !e.equal("incompatible revision not reset", after.revision, before.revision) ||
        !e.equal("incompatible count not reset", after.model_calls_used, before.model_calls_used) ||
        !world_.settle(8) ||
        !e.equal("incompatible no model dispatch", a_.pending() + b_.pending(), 0)) {
        return false;
    }
    // Roll back to the original compatible reader; the durable state is still
    // present, including the original operation. Unknown remains blocked.
    if (!world_.crash(hosts::sim::CutAppend::lost)) {
        return false;
    }
    selected_ = Policy::tool_loop;
    return world_.start() &&
           e.status(
               "inspect rollback checkpoint",
               inspect_task_progress(world_.node.kernel(), before.task_id, rollback_progress)) &&
           e.equal("rollback restores exact checkpoint", rollback_progress == progress, 1) &&
           e.equal("rollback keeps steps", task(workflow_).step_count, before.step_count) &&
           e.equal("rollback keeps original operation",
                   call(call_before.logical_call_id).operation_id, call_before.operation_id) &&
           e.equal("rollback cannot replay action", world_.service.executions(), 1);
}
bool Replacement::run(ReplacementScenario scenario) {
    if (!prepare()) {
        return false;
    }
    bool valid = false;
    switch (scenario) {
    case ReplacementScenario::baseline_checkpoint:
        valid = preserved();
        break;
    case ReplacementScenario::swap_provider_late_old_result:
    case ReplacementScenario::unknown_call_not_rebound:
    case ReplacementScenario::new_call_uses_new_binding:
        valid = provider_swap();
        break;
    case ReplacementScenario::completed_operation_returns_old:
        valid = restart(providers::mock::Variant::b, Policy::rules) && complete_workflow();
        break;
    case ReplacementScenario::swap_strategy_at_checkpoint:
        valid = compatible_swap();
        break;
    case ReplacementScenario::incompatible_private_checkpoint:
        valid = incompatible(Policy::incompatible);
        break;
    case ReplacementScenario::unknown_strategy_version:
        valid = incompatible(Policy::unknown_version);
        break;
    }
    completed_ = valid && world_.evidence.ok();
    return completed_;
}
} // namespace ayni::runtime::experiments
