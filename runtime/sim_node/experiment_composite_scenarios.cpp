#include "experiment_composite.h"
#include "experiment_execution.h"
#include <cstring>
namespace ayni::runtime::experiments {
bool Composite::oversized_round() {
    auto &e = world_.evidence;
    if (!world_.grant(1, "mock.tools") || !world_.grant(1, "notify.present")) {
        return false;
    }
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_id = text("composite.round");
    body.goal_type_version = 1;
    body.owner_instance_id = world_.instance(1);
    body.trigger = AYNI_TRIGGER_ONCE;
    goal_ = world_.submit(AYNI_INTENT_GOAL_CREATE, &body).object.id;
    if (!world_.until("oversized model output rejected",
                      [&] { return call("round.v1").outcome == AYNI_CALL_FAILED; })) {
        return false;
    }
    const auto result = call("round.v1");
    return e.equal("whole oversized round has no exposed prefix", result.tool_call_count, 0) &&
           e.equal("oversized round classified as model output", result.reason,
                   AYNI_REASON_MODEL_OUTPUT) &&
           e.status("oversized round limit status", result.status, AYNI_CAPACITY_EXCEEDED) &&
           e.equal("no first eight notifications executed", notification_.executions(), 0) &&
           e.equal("no first eight notifications submitted", notification_.submissions(), 0);
}
// C-CALL-008: the old possible effect remains blocked, with no downstream I/O.
bool Composite::unknown_action() {
    const auto before = call("increment.v1");
    auto &e = world_.evidence;
    if (!e.status("unknown action executed externally",
                  world_.service.execute(world_.node.kernel())) ||
        !world_.crash(hosts::sim::CutAppend::lost) || !world_.start() ||
        !world_.until("unknown action restored",
                      [&] { return call("increment.v1").phase == AYNI_CALL_RESULT_UNKNOWN; }) ||
        !world_.settle(16)) {
        return false;
    }
    const auto after = call("increment.v1");
    return e.equal("unknown operation retained", after.operation_id, before.operation_id) &&
           e.equal("unknown logical identity retained", after.logical_call_id,
                   before.logical_call_id) &&
           e.equal("unknown cannot replay effect", world_.service.executions(), 1) &&
           e.equal("unknown no second submission", world_.service.submissions(), 1) &&
           e.equal("unknown blocks capture", camera_.pending(), 0) &&
           e.equal("unknown blocks model", model_.pending(), 0) &&
           e.equal("unknown blocks notification", notification_.submissions(), 0);
}
// C-LIFE-022 / C-CALL-009: stop requests, late results, stopped evidence and
// resource release remain separate facts under a cancelled parent.
bool Composite::cancel_parent() {
    auto &e = world_.evidence;
    if (!e.status("late model remains observable",
                  model_.set_cancellation(fixtures::ModelProbe::Cancellation::late_success)) ||
        !analysis_pending() || !world_.physical->prepare()) {
        return false;
    }
    auto motion = initialized<ayni_goal_create_v1>();
    motion.goal_type_id = text("turn");
    motion.goal_type_version = 1;
    motion.owner_instance_id = world_.instance(1);
    motion.parent_goal_id = parent_;
    motion.trigger = AYNI_TRIGGER_ONCE;
    const auto created = world_.submit(AYNI_INTENT_GOAL_CREATE, &motion);
    if (!world_.until("child physical effect active",
                      [&] { return world_.physical->executor.samples() > 0; })) {
        return false;
    }
    camera_delivery_ = false;
    if (!change(parent_, AYNI_INTENT_GOAL_CANCEL)) {
        return false;
    }
    if (!world_.until("cancel requested before camera stop acknowledgement",
                      [&] { return subscription().phase == AYNI_SUBSCRIPTION_STOPPING; })) {
        return false;
    }
    e.fact("cancel", "capture_stop_requested", subscription().phase);
    if (!e.equal("unconfirmed capture retains its borrow", camera_.pending(), 1) ||
        !e.equal("late analysis creates no notification", notification_.submissions(), 0)) {
        return false;
    }
    if (!world_.until("cancelled model fact durable before late response", [&] {
            const auto c = call("analysis.v1");
            return c.cancel_requested && c.requires_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    model_delivery_ = true;
    camera_delivery_ = true;
    if (!world_.until("children stop without parent deadlock", [&] {
            return task().lifecycle == AYNI_TASK_CANCELLED && camera_.pending() == 0 &&
                   model_.pending() == 0 && world_.physical->info().phase == AYNI_CONTROL_STOPPED;
        })) {
        return false;
    }
    auto child = initialized<ayni_goal_info_v1>();
    if (!e.status("cancelled physical child",
                  ayni_get_goal(world_.node.kernel(), created.object.id, &child)) ||
        !e.equal("parent cancel reaches physical child", child.lifecycle, AYNI_GOAL_CANCELLED) ||
        !e.equal("cancel does not complete workflow", task().lifecycle, AYNI_TASK_CANCELLED) ||
        !e.equal("cancel does not repeat prior effect", world_.service.executions(), 1) ||
        !e.equal("cancel never presents", notification_.executions(), 0)) {
        return false;
    }
    for (const char *id : {"model.slot", "camera.slot", "notification.slot", "mock.actuator"}) {
        auto resource = initialized<ayni_resource_info_v1>();
        if (!e.status("cancelled composite resource",
                      ayni_get_resource(world_.node.kernel(), text(id), &resource)) ||
            !e.equal("all confirmed stopped resource units returned", resource.used, 0)) {
            return false;
        }
    }
    return true;
}
bool Composite::invalidate_source(SourceChange change) {
    auto &e = world_.evidence;
    if (!e.status("source invalidation retains late result",
                  model_.set_cancellation(fixtures::ModelProbe::Cancellation::late_success)) ||
        !analysis_pending()) {
        return false;
    }
    const auto old_call = call("analysis.v1");
    if (change == SourceChange::revoke) {
        if (!share_ || !world_.revoke(*share_)) {
            return false;
        }
    } else {
        auto info = initialized<ayni_memory_info_v1>();
        char bytes[128]{};
        uint32_t size = 0;
        if (!e.status("read source before delete",
                      ayni_get_memory(world_.node.kernel(), world_.owner_session(), source_, &info,
                                      bytes, sizeof(bytes), &size))) {
            return false;
        }
        const ayni_memory_target_v1 target{sizeof(target), 0, source_};
        world_.submit(AYNI_INTENT_MEMORY_DELETE, &target, 0, info.revision);
    }
    if (!world_.until("invalidated model fact durable before late response", [&] {
            const auto c = call("analysis.v1");
            return c.cancel_requested && c.requires_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    model_delivery_ = true;
    if (!world_.until("invalidated analysis drained",
                      [&] { return model_.pending() == 0 && camera_.pending() == 0; }) ||
        !world_.settle(16)) {
        return false;
    }
    if (!e.equal("stale model cannot derive memory", candidates(), 0) ||
        !e.equal("stale model cannot notify", notification_.submissions(), 0) ||
        !e.equal("stale model cannot complete workflow", task().lifecycle == AYNI_TASK_COMPLETED,
                 0) ||
        !world_.crash(hosts::sim::CutAppend::lost) || !world_.start() || !world_.settle(16)) {
        return false;
    }
    return verify_invalid_source(change) &&
           e.equal("recovery keeps deleted/shared dependency invalid", notification_.submissions(),
                   0) &&
           e.equal("no stale derived memory after recovery", candidates(), 0) &&
           e.equal("invalidated model operation retained", call("analysis.v1").operation_id,
                   old_call.operation_id);
}
bool Composite::verify_invalid_source(SourceChange change) {
    auto &e = world_.evidence;
    auto metadata = initialized<ayni_memory_info_v1>();
    uint32_t size = 0;
    const auto status = ayni_get_memory(world_.node.kernel(), world_.owner_session(), source_,
                                        &metadata, nullptr, 0, &size);
    if (change == SourceChange::erase) {
        if (status == AYNI_OK) {
            // Cleanup may retain a tombstone for diagnostics; it must never be live.
            if (!e.equal("deleted source is not live after reboot",
                         metadata.state == AYNI_MEMORY_TOMBSTONE ||
                             metadata.state == AYNI_MEMORY_SCRUBBED,
                         1) ||
                !e.equal("deleted source remains unusable", metadata.usable, 0)) {
                return false;
            }
            char body[128]{};
            return e.status("deleted source body denied after reboot",
                            ayni_get_memory(world_.node.kernel(), world_.owner_session(), source_,
                                            &metadata, body, sizeof(body), &size),
                            AYNI_EXPIRED);
        }
        return e.status("deleted source remains absent after cleanup", status, AYNI_NOT_FOUND);
    }
    auto grant = initialized<ayni_grant_info_v1>();
    if (!share_ || !e.status("source owner data retained", status) ||
        !e.status("restored shared Grant", ayni_get_grant(world_.node.kernel(), *share_, &grant)) ||
        !e.equal("shared Grant remains revoked after reboot", grant.lifecycle,
                 AYNI_GRANT_REVOKED)) {
        return false;
    }
    const auto parameters = std::to_string(source_);
    auto body = initialized<ayni_goal_create_v1>();
    body.goal_type_id = text("composite.read_source");
    body.goal_type_version = 1;
    body.owner_instance_id = world_.instance(1);
    body.trigger = AYNI_TRIGGER_ONCE;
    body.parameters = {reinterpret_cast<const uint8_t *>(parameters.data()),
                       static_cast<uint32_t>(parameters.size())};
    const auto fresh = world_.submit(AYNI_INTENT_GOAL_CREATE, &body);
    if (!world_.until("fresh actor read is denied after revoked Grant recovery",
                      [&] { return task(fresh.object.id).lifecycle == AYNI_TASK_PAUSED; })) {
        return false;
    }
    return e.status("fresh actor cannot read revoked shared source",
                    task(fresh.object.id).hold_status, AYNI_DENIED);
}
bool Composite::close_generation() {
    auto &e = world_.evidence;
    if (!e.status("closed capture still receives late analysis",
                  model_.set_cancellation(fixtures::ModelProbe::Cancellation::late_success)) ||
        !analysis_pending() ||
        !e.equal("active capture before closing", subscription().phase, AYNI_SUBSCRIPTION_ACTIVE)) {
        return false;
    }
    const auto old = *opening_;
    const auto current = subscription();
    auto info = initialized<ayni_task_info_v1>();
    info = task();
    // Closing the owning Goal closes collection through the public stop path.
    if (!change(goal_, AYNI_INTENT_GOAL_PAUSE) ||
        !world_.until("capture stopped", [&] { return camera_.pending() == 0; })) {
        return false;
    }
    auto post = initialized<ayni_subscription_observation_v1>();
    post.subscription = old.subscription;
    post.sample = {sizeof(post.sample),
                   old.source_version,
                   text("mock.camera"),
                   old.subscription.generation,
                   1000,
                   world_.node.simulation()->now_us(),
                   1000,
                   AYNI_OBSERVATION_GOOD,
                   0,
                   1};
    if (!e.status("old capture tail refused", ayni_subscription_post_observation(old.sink, &post),
                  AYNI_NOT_READY)) {
        return false;
    }
    if (!world_.until("closed capture model cancellation durable", [&] {
            const auto c = call("analysis.v1");
            return c.cancel_requested && c.requires_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    model_delivery_ = true;
    if (!world_.until("closed capture late analysis drained", [&] {
            const auto c = call("analysis.v1");
            return model_.pending() == 0 && c.outcome &&
                   c.requires_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    return e.equal("closed capture actually receives late successful model", model_.executions(),
                   1) &&
           e.equal("closed capture never creates late candidate", candidates(), 0) &&
           e.equal("closed generation does not notify", notification_.submissions(), 0) &&
           e.equal("old stopped generation retained diagnostically",
                   subscription().subscription.generation, current.subscription.generation) &&
           e.equal("paused workflow identity preserved", task().task_id, info.task_id);
}
bool Composite::presentation(CompositeScenario scenario) {
    auto &e = world_.evidence;
    if (!presentation_pending()) {
        return false;
    }
    const auto old = call("present.v1");
    if (scenario == CompositeScenario::queued_not_presented_restart) {
        if (!e.equal("enqueued is not presented", notification_.executions(), 0) ||
            !e.equal("enqueued is not completed", task().lifecycle == AYNI_TASK_COMPLETED, 0) ||
            !world_.crash(hosts::sim::CutAppend::lost) || !world_.start() ||
            !world_.until("unconfirmed presentation stays unknown",
                          [&] { return call("present.v1").phase == AYNI_CALL_RESULT_UNKNOWN; })) {
            return false;
        }
        return e.equal("queued presentation original operation", call("present.v1").operation_id,
                       old.operation_id) &&
               e.equal("queued presentation does not auto replay", notification_.submissions(),
                       1) &&
               e.equal("queued presentation still requires evidence", notification_.executions(),
                       0) &&
               e.equal("queued occurrence not falsely complete",
                       task().lifecycle == AYNI_TASK_COMPLETED, 0);
    }
    if (scenario == CompositeScenario::present_unknown_pending ||
        scenario == CompositeScenario::present_failed_pending) {
        const bool unknown = scenario == CompositeScenario::present_unknown_pending;
        if (!e.status("present classified result",
                      notification_.receipt(unknown ? AYNI_CALL_ABANDONED : AYNI_CALL_FAILED,
                                            unknown ? AYNI_CALL_EFFECT_UNKNOWN
                                                    : AYNI_CALL_EFFECT_NONE)) ||
            !world_.until("presentation holds workflow",
                          [&] { return task().lifecycle == AYNI_TASK_PAUSED; }) ||
            !e.status("duplicate present receipt refused", notification_.repeat_receipt(),
                      AYNI_INVALID_STATE) ||
            !world_.settle(8)) {
            return false;
        }
        return e.equal("unsuccessful presentation cannot complete",
                       task().lifecycle == AYNI_TASK_COMPLETED, 0) &&
               e.equal("unsuccessful presentation identity retained",
                       call("present.v1").operation_id, old.operation_id);
    }
    // Observe the actual receipt publication before the later Task completion.
    world_.items_per_step = 1;
    if (!e.status("present actual effect", notification_.execute(world_.node.kernel())) ||
        !e.status("present success receipt",
                  notification_.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE)) ||
        !world_.until("present outcome durable before Task",
                      [&] {
                          const auto c = call("present.v1");
                          return c.outcome == AYNI_CALL_COMPLETED &&
                                 c.requires_seq <= world_.info().durable_seq;
                      }) ||
        !e.equal("success is durable before Task completes",
                 task().lifecycle == AYNI_TASK_COMPLETED, 0) ||
        !world_.until("presented Task completes", [&] {
            auto t = task();
            return t.lifecycle == AYNI_TASK_COMPLETED && t.commit_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    return e.status("duplicate success refused", notification_.repeat_receipt(),
                    AYNI_INVALID_STATE) &&
           world_.crash(hosts::sim::CutAppend::lost) && world_.start() && world_.settle(16) &&
           e.equal("durable successful presentation never repeats", notification_.executions(),
                   1) &&
           e.equal("durable workflow stays complete", task().lifecycle, AYNI_TASK_COMPLETED);
}
bool Composite::run(CompositeScenario scenario) {
    if (scenario == CompositeScenario::round_over_eight_rejected) {
        completed_ = oversized_round();
        return completed_;
    }
    if (scenario == CompositeScenario::revoke_during_analysis) {
        source_owner_ = 2;
    }
    if (scenario == CompositeScenario::mismatched_binding_rejected ||
        scenario == CompositeScenario::typed_schema_rejected ||
        scenario == CompositeScenario::unknown_goal_type_rejected) {
        completed_ = registration_boundaries(scenario);
        return completed_;
    }
    if (!prepare((scenario == CompositeScenario::v2_only_pauses_old_operation ||
                  scenario == CompositeScenario::goal_v2_holds_old_goal)
                     ? Preparation::prepared_action
                     : Preparation::live_action)) {
        return false;
    }
    switch (scenario) {
    case CompositeScenario::register_and_run:
        completed_ = presentation_pending() && finish();
        break;
    case CompositeScenario::unknown_step_blocks_dependents:
        completed_ = unknown_action();
        break;
    case CompositeScenario::parent_cancel_propagates:
        completed_ = cancel_parent();
        break;
    case CompositeScenario::delete_during_analysis:
        completed_ = invalidate_source(SourceChange::erase);
        break;
    case CompositeScenario::revoke_during_analysis:
        completed_ = invalidate_source(SourceChange::revoke);
        break;
    case CompositeScenario::subscription_closed_old_generation:
        completed_ = close_generation();
        break;
    case CompositeScenario::queued_not_presented_restart:
    case CompositeScenario::present_success_commits_with_task:
    case CompositeScenario::present_unknown_pending:
    case CompositeScenario::present_failed_pending:
        completed_ = presentation(scenario);
        break;
    case CompositeScenario::v2_only_pauses_old_operation:
        completed_ = version_boundary(VersionCase::incompatible);
        break;
    case CompositeScenario::resolver_reconciles_v1:
        completed_ = version_boundary(VersionCase::resolver);
        break;
    case CompositeScenario::unsupported_format_preserved:
        completed_ = storage_boundary(VersionFault::schema, 0);
        break;
    case CompositeScenario::unsupported_legacy_schema:
        completed_ = storage_boundary(VersionFault::schema, 1);
        break;
    case CompositeScenario::rollback_unreadable_state:
        completed_ = storage_boundary(VersionFault::schema, 3);
        break;
    case CompositeScenario::unsupported_record_version:
        completed_ = storage_boundary(VersionFault::required_record, UINT16_MAX);
        break;
    case CompositeScenario::goal_v2_holds_old_goal:
        completed_ = goal_version_boundary();
        break;
    case CompositeScenario::scene_v2_blocks_old_permit:
        completed_ = scene_version_boundary();
        break;
    case CompositeScenario::compat_matrix_matches:
        completed_ = compatible_current_profile();
        break;
    default:
        return world_.evidence.status("composite scenario not implemented", AYNI_UNSUPPORTED);
    }
    return completed_;
}
} // namespace ayni::runtime::experiments
