#include "work/call_propagation.h"

namespace ayni::work {

bool end_task_call(const CallTable &calls, TaskTable &tasks, uint32_t call_index,
                   uint32_t task_index, foundation::TransitionStamp stamp) {
    const auto &state = calls.state(call_index);
    const auto &task = tasks.at(task_index);
    // ADR-0086: a background receipt never finishes another foreground call.
    if (state.attachment == CallAttachment::background &&
        task.call.id != calls.identity(call_index).call_id) {
        if (state.outcome == AYNI_CALL_ABANDONED && TaskTable::is_live(task.lifecycle) &&
            task.call.end == TaskCall::End::none) {
            tasks.hold_unknown_call(task_index, stamp);
        } else if (state.outcome != AYNI_CALL_ABANDONED) {
            tasks.settle_call_holds(task_index, stamp);
        }
        return true;
    }
    if (state.outcome == AYNI_CALL_ABANDONED && task.call.end == TaskCall::End::none) {
        tasks.hold_unknown_call(task_index, stamp);
        return true;
    }
    if (task.call.end == TaskCall::End::none && state.outcome == AYNI_CALL_COMPLETED) {
        const auto &step = calls.identity(call_index).step_key;
        if (!tasks.add_step(task_index, {step.data(), step.size()}, stamp)) {
            return false;
        }
    }
    tasks.finish_call(task_index, stamp);
    return true;
}

bool reconcile_task_call(const CallTable &calls, TaskTable &tasks, uint32_t call_index,
                         uint32_t task_index, foundation::TransitionStamp stamp) {
    if (tasks.at(task_index).call.id != calls.identity(call_index).call_id &&
        calls.state(call_index).outcome != AYNI_CALL_ABANDONED) {
        tasks.settle_call_holds(task_index, stamp);
        return true;
    }
    return calls.state(call_index).outcome == AYNI_CALL_ABANDONED ||
           tasks.at(task_index).call.id != calls.identity(call_index).call_id ||
           end_task_call(calls, tasks, call_index, task_index, stamp);
}

ayni_status_t skip_queued_call(CallTable &calls, TaskTable &tasks, uint32_t call_index,
                               uint32_t task_index, ActionStamp stamp) {
    const auto status = calls.skip_before_dispatch(call_index, stamp);
    if (status == AYNI_OK) {
        tasks.skip_queued_call(task_index, stamp.transition);
    }
    return status;
}

} // namespace ayni::work
