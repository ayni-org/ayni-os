#include "experiment_execution.h"
#include "experiment_shared.h"
namespace ayni::runtime::experiments {
namespace {
constexpr int64_t before_due_utc = 86400, due_utc = 86460, after_quiet_utc = 90000;
}
bool SharedServices::update_presentation_window() {
    const auto lifecycle = world_.lifecycle();
    if (lifecycle != AYNI_LIFECYCLE_RUNNING && lifecycle != AYNI_LIFECYCLE_CONFIGURING) {
        return true;
    }
    auto sample = initialized<ayni_utc_sample_v1>();
    const auto clock = world_.node.clock();
    clock.utc_now(clock.context, &sample);
    bool available = false;
    auto status = quiet_.allows(sample, available);
    if (status == AYNI_OK) {
        status = ayni_set_resource_health(world_.node.kernel(), text("notification.window"),
                                          available ? AYNI_RESOURCE_ONLINE : AYNI_RESOURCE_OFFLINE);
    }
    return status == AYNI_OK || world_.evidence.status("set current presentation window", status);
}
uint64_t SharedServices::reminder() {
    world_.node.set_utc(before_due_utc);
    applications::assistant::Schedule schedule;
    if (!world_.evidence.status(
            "parse actual reminder",
            applications::assistant::parse_schedule(
                "remind once 1970-01-02T00:01 UTC0 synthetic reminder", schedule))) {
        return 0;
    }
    applications::assistant::Encoded encoded;
    auto body = initialized<ayni_goal_create_v1>();
    if (!world_.evidence.status(
            "prepare actual reminder",
            applications::assistant::prepare_goal(schedule, world_.instance(1), encoded, body))) {
        return 0;
    }
    const auto result = world_.submit(AYNI_INTENT_GOAL_CREATE, &body);
    if (!world_.until("reminder durable before becoming due",
                      [&] { return result.commit_seq <= world_.info().durable_seq; })) {
        return 0;
    }
    return result.object.id;
}
// C-PROD-010/013/014/020: the real reminder policy consumes the original
// notification result; a receipt is distinct from enqueue and external effect.
bool SharedServices::present(PresentationCase mode) {
    auto &e = world_.evidence;
    if (!world_.grant(1, "notify.present")) {
        return false;
    }
    uint64_t observe = 0;
    if (mode == PresentationCase::pause) {
        if (!world_.grant(1, "mock.observe") || !world_.grant(1, "mock.text")) {
            return false;
        }
        applications::assistant::Parameters p;
        p.kind = applications::assistant::GoalKind::scene_observe;
        applications::assistant::Encoded bytes;
        auto body = initialized<ayni_goal_create_v1>();
        if (!e.status("prepare autonomous observation",
                      applications::assistant::prepare_goal(p, world_.instance(1), 0, nullptr,
                                                            bytes, body))) {
            return false;
        }
        observe = world_.submit(AYNI_INTENT_GOAL_CREATE, &body).object.id;
        if (!world_.until("autonomous camera active", [&] {
                auto lease = initialized<ayni_subscription_info_v1>();
                return last_opening_ &&
                       ayni_get_subscription(world_.node.kernel(),
                                             last_opening_->subscription.subscription_id,
                                             &lease) == AYNI_OK &&
                       lease.phase == AYNI_SUBSCRIPTION_ACTIVE;
            })) {
            return false;
        }
    }
    if (mode == PresentationCase::estop || mode == PresentationCase::pause) {
        if (!e.equal("physical deployment present", world_.physical != nullptr, 1) ||
            !world_.physical->prepare(2)) {
            return false;
        }
        auto motion = initialized<ayni_goal_create_v1>();
        motion.goal_type_id = text("turn");
        motion.goal_type_version = 1;
        motion.owner_instance_id = world_.instance(2);
        motion.trigger = AYNI_TRIGGER_ONCE;
        motion.pause_group = applications::assistant::autonomous_group;
        world_.submit(AYNI_INTENT_GOAL_CREATE, &motion);
        if (!world_.until("physical output precedes stop",
                          [&] { return world_.physical->executor.samples() > 0; })) {
            return false;
        }
    }
    if (mode == PresentationCase::quiet) {
        quiet_.enabled = true;
        quiet_.start_minute = 0;
        quiet_.end_minute = 60;
        quiet_.timezone = "UTC0";
    }
    const auto goal = reminder();
    if (!goal) {
        return false;
    }
    if (mode == PresentationCase::pause) {
        const ayni_pause_group_set_v1 body{sizeof(body), applications::assistant::autonomous_group,
                                           1, 0};
        world_.submit(AYNI_INTENT_PAUSE_GROUP_SET, &body);
        if (!world_.until("pause ends autonomous capture and motion", [&] {
                return camera_.pending() == 0 &&
                       world_.physical->info().phase == AYNI_CONTROL_STOPPED;
            })) {
            return false;
        }
        auto activity = applications::assistant::Activity{};
        if (!e.status("assistant pause projection",
                      applications::assistant::project(world_.node.kernel(), activity)) ||
            !e.equal("assistant projection keeps pause", activity.autonomy_paused, 1)) {
            return false;
        }
    }
    world_.node.set_utc(due_utc);
    if (mode == PresentationCase::quiet) {
        if (!world_.until("quiet reminder available",
                          [&] { return call(goal).wait_reason == AYNI_CALL_WAIT_OFFLINE; }) ||
            !e.equal("quiet window has no presentation", notification_.submissions(), 0) ||
            !e.equal("quiet reminder available projection",
                     static_cast<uint32_t>(applications::assistant::presentation(call(goal))),
                     static_cast<uint32_t>(applications::assistant::Presentation::available))) {
            return false;
        }
        world_.node.set_utc(after_quiet_utc);
    }
    if (!world_.until("reminder enqueued for presenter",
                      [&] { return notification_.pending() == 1; }) ||
        !e.equal("enqueue alone does not complete occurrence",
                 task(goal).lifecycle == AYNI_TASK_COMPLETED, 0)) {
        return false;
    }
    const auto original = call(goal);
    if (mode == PresentationCase::estop) {
        if (!world_.physical->estop() ||
            !e.status("independent stop cycle", world_.physical->executor.advance()) ||
            !e.equal("reminder retained after stop", notification_.pending(), 1)) {
            return false;
        }
    }
    if (mode == PresentationCase::failed || mode == PresentationCase::unknown) {
        const bool unknown = mode == PresentationCase::unknown;
        if (!e.status("presentation terminal evidence",
                      notification_.receipt(unknown ? AYNI_CALL_ABANDONED : AYNI_CALL_FAILED,
                                            unknown ? AYNI_CALL_EFFECT_UNKNOWN
                                                    : AYNI_CALL_EFFECT_NONE)) ||
            !world_.until("failed presentation pending",
                          [&] { return task(goal).lifecycle == AYNI_TASK_PAUSED; })) {
            return false;
        }
        return e.equal("presentation remains pending", task(goal).lifecycle == AYNI_TASK_COMPLETED,
                       0) &&
               e.equal("failure classification",
                       static_cast<uint32_t>(applications::assistant::presentation(call(goal))),
                       static_cast<uint32_t>(unknown
                                                 ? applications::assistant::Presentation::unknown
                                                 : applications::assistant::Presentation::failed));
    }
    if (!e.status("presenter performs one real simulated presentation",
                  notification_.execute(world_.node.kernel()))) {
        return false;
    }
    if (mode == PresentationCase::lost) {
        if (!world_.crash(hosts::sim::CutAppend::lost) || !world_.start()) {
            return false;
        }
        const auto restored = call(goal);
        if (!e.equal("lost receipt retains original operation", restored.operation_id,
                     original.operation_id) ||
            !e.equal("lost receipt is unknown", restored.phase, AYNI_CALL_RESULT_UNKNOWN)) {
            return false;
        }
        const ayni_call_query_result_v1 body{sizeof(body), 0, restored.logical_call_id};
        world_.submit(AYNI_INTENT_CALL_QUERY_RESULT, &body, 0, restored.query_revision);
        if (!world_.until("presentation reconciliation dispatched",
                          [&] { return notification_.pending_queries() == 1; }) ||
            !e.status("authoritative presentation found",
                      notification_.answer_query(AYNI_QUERY_RESULT_FOUND))) {
            return false;
        }
    } else if (!e.status("presentation success returned",
                         notification_.receipt(AYNI_CALL_COMPLETED, AYNI_CALL_EFFECT_COMPLETE))) {
        return false;
    }
    if (!world_.until("successful presentation completes original occurrence durably", [&] {
            const auto done = task(goal);
            return done.lifecycle == AYNI_TASK_COMPLETED &&
                   done.commit_seq <= world_.info().durable_seq;
        })) {
        return false;
    }
    const auto completed = task(goal);
    if (mode == PresentationCase::duplicate &&
        !e.status("duplicate receipt rejected", notification_.repeat_receipt(),
                  AYNI_INVALID_STATE)) {
        return false;
    }
    if (!world_.crash(hosts::sim::CutAppend::lost) || !world_.start() || !world_.settle()) {
        return false;
    }
    return e.equal("confirmed reminder never replays", notification_.executions(), 1) &&
           e.equal("one notification delivery across reboot", notification_.submissions(), 1) &&
           e.equal("one original occurrence Task", task(goal).task_id, completed.task_id) &&
           e.equal("completed reminder remains completed", task(goal).lifecycle,
                   AYNI_TASK_COMPLETED) &&
           e.equal("autonomous observation separate from reminder", observe == 0 || observe != goal,
                   1);
}
} // namespace ayni::runtime::experiments
