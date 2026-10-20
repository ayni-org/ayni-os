#include "kernel/intent/decision.h"

#include "catalog/catalog_id.h"

#include <cstring>
#include <new>

namespace ayni::kernel {
namespace {

static_assert(alignof(ayni_policy_intent_v1) <= alignof(ayni_task_step_v1),
              "the intents after the step views are aligned");

// What a decision does to its task (ayni_decide_fn): ends it by end, or
// leaves it READY, after committing its steps and private checkpoint either
// way; or holds it with hold_status and hold_reason.
struct Verdict {
    enum class Kind { end, progress, hold };
    Kind kind;
    work::TaskChange end;
    ayni_status_t hold_status;
    ayni_reason_t hold_reason;
};

Verdict hold(ayni_status_t status, ayni_reason_t reason) {
    return {Verdict::Kind::hold, work::TaskChange::complete, status, reason};
}

bool is_implemented(ayni_policy_intent_kind_t kind) {
    return kind == AYNI_POLICY_REPORT_PROGRESS || kind == AYNI_POLICY_REPORT_COMPLETION ||
           kind == AYNI_POLICY_REPORT_FAILURE;
}

// A view of at most step_key_size bytes, so CatalogId::is_valid rejects a
// size past the array before reading it.
ayni_string_v1 step_key_of(const ayni_policy_intent_v1 &intent) {
    return {intent.step_key, intent.step_key_size};
}

// Whether a REPORT_PROGRESS intent before index commits key.
bool commits_earlier(const DecisionBuffer &decisions, uint32_t index, ayni_string_v1 key) {
    for (uint32_t earlier = 0; earlier < index; ++earlier) {
        const ayni_policy_intent_v1 &intent = decisions.at(earlier);
        if (intent.kind == AYNI_POLICY_REPORT_PROGRESS && intent.step_key_size == key.size &&
            std::memcmp(intent.step_key, key.data, key.size) == 0) {
            return true;
        }
    }
    return false;
}

// The checks of ayni_decide_fn on what decide returned and the sizes it
// set, in its order, against the kernel's copy of the strategy's limits.
Verdict judge_output(ayni_status_t status, uint32_t intent_count, uint32_t checkpoint_size,
                     const catalog::Strategy &strategy) {
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
    if (intent_count > strategy.max_intents_per_call ||
        checkpoint_size > strategy.max_checkpoint_bytes) {
        return hold(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT);
    }
    if (intent_count == 0U) {
        return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_NO_PROGRESS);
    }
    return {Verdict::Kind::progress, work::TaskChange::complete, AYNI_OK, AYNI_REASON_NONE};
}

// The checks of ayni_decide_fn on each intent in turn, once the output
// passed judge_output. A step must be new to the task and to the decision,
// and fit in the task's steps.
Verdict judge_intents(const DecisionBuffer &decisions, uint32_t intent_count,
                      const work::TaskTable &tasks, uint32_t task_index) {
    const work::Task &task = tasks.at(task_index);
    Verdict verdict{Verdict::Kind::progress, work::TaskChange::complete, AYNI_OK, AYNI_REASON_NONE};
    uint32_t step_count = task.step_count;
    for (uint32_t index = 0; index < intent_count; ++index) {
        const ayni_policy_intent_v1 &intent = decisions.at(index);
        if (intent.struct_size != sizeof(ayni_policy_intent_v1)) {
            return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT);
        }
        if (!is_implemented(intent.kind)) {
            return hold(AYNI_UNSUPPORTED, AYNI_REASON_INTENT_UNSUPPORTED);
        }
        if (intent.expected_revision != task.revision) {
            return hold(AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION);
        }
        if (intent.kind == AYNI_POLICY_REPORT_PROGRESS) {
            const ayni_string_v1 key = step_key_of(intent);
            if (!catalog::CatalogId::is_valid(key)) {
                return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT);
            }
            if (tasks.has_step(task_index, key) || commits_earlier(decisions, index, key)) {
                return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_STEP_TAKEN);
            }
            if (step_count == tasks.step_capacity()) {
                return hold(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_STEP_CAPACITY);
            }
            ++step_count;
            continue;
        }
        if (intent.step_key_size != 0U || verdict.kind == Verdict::Kind::end) {
            return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT);
        }
        verdict.kind = Verdict::Kind::end;
        verdict.end = intent.kind == AYNI_POLICY_REPORT_COMPLETION ? work::TaskChange::complete
                                                                   : work::TaskChange::fail;
    }
    return verdict;
}

ayni_task_snapshot_v1 snapshot_of(const WorkModel &model, DecisionBuffer &decisions,
                                  uint32_t task_index, uint64_t now_us) {
    const work::Task &task = model.tasks.at(task_index);
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
    if (task.checkpoint_writer != work::TaskTable::no_writer) {
        const catalog::Strategy &writer = model.strategies.at(task.checkpoint_writer);
        snapshot.checkpoint.strategy_id = {writer.id.data(), writer.id.size()};
        snapshot.checkpoint.strategy_version = writer.version;
    }
    snapshot.checkpoint.state = model.tasks.checkpoint(task_index);
    ayni_task_step_v1 *steps = decisions.steps();
    for (uint32_t step = 0; step < task.step_count; ++step) {
        const catalog::CatalogId &key = model.tasks.step(task_index, step);
        steps[step] = {{key.data(), key.size()}};
    }
    snapshot.step_count = task.step_count;
    snapshot.step_capacity = model.tasks.step_capacity();
    snapshot.steps = steps;
    return snapshot;
}

// Commits what a decision that does not hold the task brings with it: the
// steps of its REPORT_PROGRESS intents, in their order, and the private
// checkpoint its strategy, writer, left in the buffer.
void record(work::TaskTable &tasks, uint32_t task_index, const DecisionBuffer &decisions,
            uint32_t intent_count, uint32_t writer, uint32_t checkpoint_size) {
    for (uint32_t index = 0; index < intent_count; ++index) {
        const ayni_policy_intent_v1 &intent = decisions.at(index);
        if (intent.kind == AYNI_POLICY_REPORT_PROGRESS) {
            // Cannot fail: judge_intents checked each step.
            static_cast<void>(tasks.add_step(task_index, step_key_of(intent)));
        }
    }
    tasks.set_checkpoint(task_index, writer, decisions.checkpoint(checkpoint_size));
}

} // namespace

std::size_t DecisionBuffer::storage_size(const Limits &limits) {
    return sizeof(ayni_task_step_v1) * limits.step_capacity +
           sizeof(ayni_policy_intent_v1) * limits.intent_capacity + limits.checkpoint_max_bytes;
}

DecisionBuffer::DecisionBuffer(void *storage, const Limits &limits)
    : steps_(static_cast<ayni_task_step_v1 *>(storage)),
      intents_(reinterpret_cast<ayni_policy_intent_v1 *>(steps_ + limits.step_capacity)),
      checkpoint_(reinterpret_cast<uint8_t *>(intents_ + limits.intent_capacity)),
      intent_capacity_(limits.intent_capacity) {
    for (uint32_t index = 0; index < limits.step_capacity; ++index) {
        new (steps_ + index) ayni_task_step_v1{};
    }
    for (uint32_t index = 0; index < limits.intent_capacity; ++index) {
        new (intents_ + index) ayni_policy_intent_v1{};
    }
}

ayni_policy_intent_v1 *DecisionBuffer::clear(uint32_t count) {
    for (uint32_t index = 0; index < count; ++index) {
        intents_[index] = ayni_policy_intent_v1{};
    }
    return intents_;
}

uint8_t *DecisionBuffer::load_checkpoint(ayni_bytes_v1 state, uint32_t capacity) {
    if (state.size > 0U) {
        std::memcpy(checkpoint_, state.data, state.size);
    }
    if (capacity > state.size) {
        std::memset(checkpoint_ + state.size, 0, capacity - state.size);
    }
    return checkpoint_;
}

bool decide_next(WorkModel &model, DecisionBuffer &decisions, uint64_t ready_before,
                 uint64_t now_us) {
    uint32_t index = 0;
    if (!model.tasks.next_ready(ready_before, index)) {
        return false;
    }
    const work::Task &task = model.tasks.at(index);
    const uint32_t strategy_index =
        model.goal_types.at(model.goals.at(task.goal_index).goal_type_index).strategy_index;
    const catalog::Strategy &strategy = model.strategies.at(strategy_index);

    const ayni_task_snapshot_v1 snapshot = snapshot_of(model, decisions, index, now_us);
    ayni_policy_output_v1 output{};
    output.struct_size = sizeof(output);
    output.intent_capacity = strategy.max_intents_per_call;
    output.intents = decisions.clear(strategy.max_intents_per_call);
    // Only this strategy decides the task, so it wrote the checkpoint
    // within its own limit.
    output.checkpoint_capacity = strategy.max_checkpoint_bytes;
    output.checkpoint_size = snapshot.checkpoint.state.size;
    output.checkpoint =
        decisions.load_checkpoint(snapshot.checkpoint.state, strategy.max_checkpoint_bytes);
    // Nothing changes while the strategy runs (C-POLICY-001), so the task
    // it decides is still the one it saw.
    model.calling_out = true;
    const ayni_status_t status = strategy.decide(strategy.ctx, &snapshot, &output);
    model.calling_out = false;

    Verdict verdict = judge_output(status, output.intent_count, output.checkpoint_size, strategy);
    if (verdict.kind != Verdict::Kind::hold) {
        verdict = judge_intents(decisions, output.intent_count, model.tasks, index);
    }
    work::WorkTables tables = model.tables();
    if (verdict.kind == Verdict::Kind::end && work::task_end_exhausts(tables, index, verdict.end)) {
        verdict = hold(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED);
    }
    const foundation::TransitionStamp stamp = model.transitions.next();
    switch (verdict.kind) {
    case Verdict::Kind::end:
        record(model.tasks, index, decisions, output.intent_count, strategy_index,
               output.checkpoint_size);
        work::end_task(tables, index, verdict.end, stamp);
        // An end frees a place in the task table and may end goals.
        model.due_work_changed = true;
        break;
    case Verdict::Kind::progress:
        record(model.tasks, index, decisions, output.intent_count, strategy_index,
               output.checkpoint_size);
        model.tasks.progress(index, stamp);
        break;
    case Verdict::Kind::hold:
        model.tasks.hold(index, verdict.hold_status, verdict.hold_reason, stamp);
        // A hold keeps the goal from deriving (work::derivation_blocker).
        model.due_work_changed = true;
        break;
    }
    return true;
}

bool awaits_decision(const WorkModel &model) {
    return model.tasks.has_ready();
}

} // namespace ayni::kernel
