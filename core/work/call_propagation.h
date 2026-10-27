// How a call's facts reach the task that made it (ADR-0024, ADR-0029,
// ADR-0045). The call table keeps the call's facts and the task table the
// task's. The kernel's call runtime records a call fact and, in the same
// transition stamp, calls one of these, so the task follows the call by one
// rule wherever the fact came from: a Provider result, a local cancel, a
// refused submission, a restore or a reconciliation.

#ifndef AYNI_CORE_WORK_CALL_PROPAGATION_H
#define AYNI_CORE_WORK_CALL_PROPAGATION_H

#include "foundation/transition_stamp.h"
#include "work/call/call_table.h"
#include "work/task/task_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::work {

// The call call_index of task task_index has the terminal fact recorded in
// stamp. For a selected foreground call, ABANDONED holds the task until a
// reconciliation or a cancel ends it. Otherwise the task's call ends,
// keeping a user pause; a COMPLETED call first adds its step, while other
// known outcomes keep the step identity in the call table only, so a
// strategy must inspect the result and choose a new step before trying
// again. A task whose end was already requested only ends its call.
// Returns false, changing nothing, when the task table refuses the step.
// Detached background subscriptions update their own row without ending or
// adding steps to another foreground call owned by the same Task.
bool end_task_call(const CallTable &calls, TaskTable &tasks, uint32_t call_index,
                   uint32_t task_index, foundation::TransitionStamp stamp);

// After the call table applied a reconciliation answer to the ABANDONED call
// call_index: an answer that left it UNKNOWN keeps the task held, and a task
// that no longer selects the call has moved on without it; otherwise as
// end_task_call.
bool reconcile_task_call(const CallTable &calls, TaskTable &tasks, uint32_t call_index,
                         uint32_t task_index, foundation::TransitionStamp stamp);

// The queued background call call_index waited too long for its resources:
// it is SKIPPED, never having reached a Provider, and the task's run ends
// without advancing its goal's next occurrence, which was taken when the
// task was derived. The task changes only when the call table returns AYNI_OK.
ayni_status_t skip_queued_call(CallTable &calls, TaskTable &tasks, uint32_t call_index,
                               uint32_t task_index, ActionStamp stamp);

} // namespace ayni::work

#endif // AYNI_CORE_WORK_CALL_PROPAGATION_H
