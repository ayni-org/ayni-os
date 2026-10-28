// E1 S5 / C-RUN-001..004: per-Task model consumption is independent of the
// one shared concurrency slot; ordinary resume and restart never reset it.
#include "experiment_shared.h"
#include "experiment_support.h"
namespace ayni::runtime::experiments {
uint64_t SharedServices::exhaust_model_task() {
    if (!world_.grant(1, "mock.text")) {
        return 0;
    }
    const auto goal = create(applications::normal_fixtures::GoalKind::repeat_model, 1);
    if (!world_.until("two-model-call limit pauses Task", [&] {
            const auto current = task(goal);
            return current.lifecycle == AYNI_TASK_PAUSED && current.model_call_limit_reached == 1 &&
                   current.commit_seq <= world_.info().durable_seq;
        })) {
        return 0;
    }
    const auto current = task(goal);
    if (!world_.evidence.equal("accepted model count", current.model_calls_used, 2) ||
        !world_.evidence.equal("initial per-Task limit", current.model_call_limit, 2) ||
        !world_.evidence.equal("classified runtime hold", current.hold_reason,
                               AYNI_REASON_TASK_MODEL_CALL_LIMIT) ||
        !world_.evidence.equal("limit releases model concurrency slot", model_slot().used, 0) ||
        !world_.evidence.equal("no third model effect",
                               model_a_.executions() + model_b_.executions(), 2)) {
        return 0;
    }
    return goal;
}
bool SharedServices::model_limit() {
    const auto a = exhaust_model_task();
    if (!a) {
        return false;
    }
    auto current = initialized<ayni_goal_info_v1>();
    if (!world_.evidence.status("get limited Goal",
                                ayni_get_goal(world_.node.kernel(), a, &current))) {
        return false;
    }
    const ayni_goal_target_v1 target{sizeof(target), 0, a};
    world_.submit(AYNI_INTENT_GOAL_PAUSE, &target, 0, current.revision);
    if (!world_.evidence.status("get paused Goal",
                                ayni_get_goal(world_.node.kernel(), a, &current))) {
        return false;
    }
    world_.submit(AYNI_INTENT_GOAL_RESUME, &target, 0, current.revision);
    if (!world_.evidence.equal("ordinary resume preserves count", task(a).model_calls_used, 2) ||
        !world_.evidence.equal("ordinary resume preserves independent limit hold",
                               task(a).model_call_limit_reached, 1) ||
        !world_.grant(2, "mock.text")) {
        return false;
    }
    const auto b = create_chat(2);
    return world_.until("other Agent ToolLoop still completes",
                        [&] { return task(b).lifecycle == AYNI_TASK_COMPLETED; }) &&
           world_.evidence.equal("A remains limited", task(a).model_calls_used, 2) &&
           world_.evidence.equal("other Task has independent count", task(b).model_calls_used, 1);
}
bool SharedServices::restart_limit() {
    const auto goal = exhaust_model_task();
    if (!goal) {
        return false;
    }
    const auto before = task(goal);
    if (!world_.crash(hosts::sim::CutAppend::lost) || !world_.start()) {
        return false;
    }
    const auto after = task(goal);
    return world_.evidence.equal("same Task after recovery", after.task_id, before.task_id) &&
           world_.evidence.equal("model count persists", after.model_calls_used,
                                 before.model_calls_used) &&
           world_.evidence.equal("model limit persists", after.model_call_limit,
                                 before.model_call_limit) &&
           world_.evidence.equal("runtime hold persists", after.model_call_limit_reached, 1) &&
           world_.settle() &&
           world_.evidence.equal("restart cannot reexecute a limited Task",
                                 model_a_.executions() + model_b_.executions(), 2);
}
} // namespace ayni::runtime::experiments
