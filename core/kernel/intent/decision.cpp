#include "kernel/intent/decision.h"

#include <new>

namespace ayni::kernel {
namespace {

// What a decision does to its task: ends it by end, or holds it with
// hold_status and hold_reason.
struct Verdict {
    bool ends;
    work::TaskChange end;
    ayni_status_t hold_status;
    ayni_reason_t hold_reason;
};

Verdict hold(ayni_status_t status, ayni_reason_t reason) {
    return {false, work::TaskChange::complete, status, reason};
}

bool is_report(ayni_policy_intent_kind_t kind) {
    return kind == AYNI_POLICY_REPORT_COMPLETION || kind == AYNI_POLICY_REPORT_FAILURE;
}

// The checks of ayni_decide_fn, in its order. Every intent kind this library
// implements ends the task, so a second intent always contradicts the first.
Verdict judge(ayni_status_t status, uint32_t intent_count, uint32_t intent_capacity,
              const DecisionBuffer &decisions, uint32_t revision) {
    if (status == AYNI_CAPACITY_EXCEEDED) {
        return hold(status, AYNI_REASON_POLICY_OUTPUT_LIMIT);
    }
    // Statuses are failures below AYNI_OK; anything above is no status, and
    // a hold keeps a failure (TaskTable::hold).
    if (status > AYNI_OK) {
        return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_NONE);
    }
    if (status != AYNI_OK) {
        return hold(status, AYNI_REASON_NONE);
    }
    if (intent_count > intent_capacity) {
        return hold(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT);
    }
    if (intent_count == 0U) {
        return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_NO_PROGRESS);
    }
    Verdict verdict = hold(AYNI_OK, AYNI_REASON_NONE);
    for (uint32_t index = 0; index < intent_count; ++index) {
        const ayni_policy_intent_v1 &intent = decisions.at(index);
        if (intent.struct_size != sizeof(ayni_policy_intent_v1)) {
            return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT);
        }
        if (!is_report(intent.kind)) {
            return hold(AYNI_UNSUPPORTED, AYNI_REASON_INTENT_UNSUPPORTED);
        }
        if (intent.expected_revision != revision) {
            return hold(AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION);
        }
        if (verdict.ends) {
            return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT);
        }
        verdict.ends = true;
        verdict.end = intent.kind == AYNI_POLICY_REPORT_COMPLETION ? work::TaskChange::complete
                                                                   : work::TaskChange::fail;
    }
    return verdict;
}

ayni_task_snapshot_v1 snapshot_of(const WorkModel &model, const work::Task &task, uint64_t now_us) {
    const catalog::GoalType &type =
        model.goal_types.at(model.goals.at(task.goal_index).goal_type_index);
    ayni_task_snapshot_v1 snapshot{};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.revision = task.revision;
    snapshot.task_id = task.task_id;
    snapshot.goal_id = task.goal_id;
    snapshot.owner_instance_id = task.owner_instance_id;
    snapshot.due_us = task.due_us;
    snapshot.now_us = now_us;
    snapshot.occurrence = task.occurrence;
    snapshot.occurrence_count = task.occurrence_count;
    snapshot.goal_type_version = type.version;
    snapshot.goal_type_id = {type.id.data(), type.id.size()};
    snapshot.goal_parameters = model.goals.parameters(task.goal_index);
    return snapshot;
}

} // namespace

DecisionBuffer::DecisionBuffer(void *storage, uint32_t capacity)
    : intents_(static_cast<ayni_policy_intent_v1 *>(storage)), capacity_(capacity) {
    for (uint32_t index = 0; index < capacity; ++index) {
        new (intents_ + index) ayni_policy_intent_v1{};
    }
}

ayni_policy_intent_v1 *DecisionBuffer::clear(uint32_t count) {
    for (uint32_t index = 0; index < count; ++index) {
        intents_[index] = ayni_policy_intent_v1{};
    }
    return intents_;
}

bool decide_next(WorkModel &model, DecisionBuffer &decisions, uint64_t ready_before,
                 uint64_t now_us) {
    uint32_t index = 0;
    if (!model.tasks.next_ready(ready_before, index)) {
        return false;
    }
    const work::Task &task = model.tasks.at(index);
    const catalog::GoalType &type =
        model.goal_types.at(model.goals.at(task.goal_index).goal_type_index);
    const catalog::Strategy &strategy = model.strategies.at(type.strategy_index);

    const ayni_task_snapshot_v1 snapshot = snapshot_of(model, task, now_us);
    ayni_policy_output_v1 output{};
    output.struct_size = sizeof(output);
    output.intent_capacity = strategy.max_intents_per_call;
    output.intents = decisions.clear(strategy.max_intents_per_call);
    // Nothing changes while the strategy runs (C-POLICY-001), so the task
    // it decides is still the one it saw.
    model.calling_out = true;
    const ayni_status_t status = strategy.decide(strategy.ctx, &snapshot, &output);
    model.calling_out = false;

    Verdict verdict =
        judge(status, output.intent_count, strategy.max_intents_per_call, decisions, task.revision);
    work::WorkTables tables = model.tables();
    if (verdict.ends && work::task_end_exhausts(tables, index, verdict.end)) {
        verdict = hold(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED);
    }
    const foundation::TransitionStamp stamp = model.transitions.next();
    if (verdict.ends) {
        work::end_task(tables, index, verdict.end, stamp);
    } else {
        model.tasks.hold(index, verdict.hold_status, verdict.hold_reason, stamp);
    }
    // An end frees a place in the task table and may end goals; a hold
    // keeps the goal from deriving (work::derivation_blocker).
    model.due_work_changed = true;
    return true;
}

bool awaits_decision(const WorkModel &model) {
    return model.tasks.has_ready();
}

} // namespace ayni::kernel
