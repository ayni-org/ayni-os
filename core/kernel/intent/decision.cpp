#include "kernel/intent/decision.h"

#include "catalog/catalog_id.h"
#include "kernel/call_runtime.h"

#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace ayni::kernel {
namespace {

// Two bits per Goal slot encode unknown, available or held. All cells start
// unknown; no result survives this synchronous immutable owner-thread query.
// At 1024 Goal slots this costs 256 B of stack, never caller arena or heap.
constexpr uint32_t compatibility_state_bits = 2;
constexpr uint32_t compatibility_word_bits = std::numeric_limits<uint64_t>::digits;
constexpr uint32_t compatibility_states_per_word =
    compatibility_word_bits / compatibility_state_bits;
constexpr uint32_t compatibility_word_count =
    (AYNI_GOAL_CAPACITY_MAX + compatibility_states_per_word - 1U) / compatibility_states_per_word;
constexpr uint64_t compatibility_state_mask = (uint64_t{1} << compatibility_state_bits) - 1U;
enum class Compatibility : uint64_t { unknown = 0, available = 1, held = 2 };

// Borrowed admission context and owned static-compatibility memo for one
// readiness query. This never caches authority, expiry, Gate or consume results.
struct DecisionAdmission {
    const WorkModel &model; // Owner state, unchanged for the selection call.
    uint64_t durable_seq;   // Durable-prefix sample; no cross-boot comparison.
    // Query-local sample: false iff no retained Grant has a boot compatibility
    // hold. The owner model cannot change during this synchronous query.
    bool inspect_compatibility;
    // Packed Compatibility cells by retained Goal index; zero means unknown.
    mutable std::array<uint64_t, compatibility_word_count> compatibility{};

    DecisionAdmission(const WorkModel &owner, uint64_t durable)
        : model(owner), durable_seq(durable),
          inspect_compatibility(owner.grants.has_compatibility_holds()) {}

    // Share each Goal's static reference check across its READY Tasks. Slot
    // identities cannot change during the query; a later query starts fresh.
    bool references_compatible(uint32_t goal_index) const {
        auto &word = compatibility[goal_index / compatibility_states_per_word];
        const auto shift = (goal_index % compatibility_states_per_word) * compatibility_state_bits;
        const auto state = static_cast<Compatibility>((word >> shift) & compatibility_state_mask);
        if (state != Compatibility::unknown) {
            return state == Compatibility::available;
        }
        const bool available =
            !model.goal_authority.contains(goal_index, model.goals.at(goal_index).goal_id) ||
            work::goal_references_compatible(model.goal_authority.at(goal_index), model.grants);
        // Only unknown cells are written, so OR preserves all other Goal cells.
        word |= static_cast<uint64_t>(available ? Compatibility::available : Compatibility::held)
                << shift;
        return available;
    }
};

// Paused or unconfirmed replacement bindings wait without spinning READY.
// Missing evidence reaches the consumption check, which records its refusal.
bool admission_ready(const void *context, const work::Task &task) {
    const auto &query = *static_cast<const DecisionAdmission *>(context);
    const auto &authority = query.model.task_authority;
    if (authority.contains(task.slot, task.task_id) &&
        authority.at(task.slot).phase == work::TaskAuthorityPhase::paused) {
        return false;
    }
    const auto &goal = query.model.goals.at(task.goal_index);
    if (query.model.goal_authority.contains(task.goal_index, goal.goal_id) &&
        work::replacement_binding_pending(query.model.goal_authority.at(task.goal_index).binding,
                                          goal.created.commit_seq, query.durable_seq)) {
        return false;
    }
    // Catalog/subject recovery holds are static for this boot. Keep their
    // availability out of persistent authorization and policy failures;
    // registration cannot change during a strategy callback (C-LIFE-003/C-POLICY-001).
    return !query.inspect_compatibility ||
           query.model.goals.visit_lineage(
               task.goal_index, [&](uint32_t at) { return query.references_compatible(at); });
}

static_assert(alignof(ayni_policy_intent_v1) <= alignof(ayni_task_step_v1),
              "the intents after the step views are aligned");

// What a decision does to its task (ayni_decide_fn): ends it by end, or
// leaves it READY, after committing its steps and private checkpoint either
// way; or holds it with hold_status and hold_reason.
struct Verdict {
    enum class Kind { end, progress, hold, call };
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
           kind == AYNI_POLICY_REPORT_FAILURE || kind == AYNI_POLICY_REQUEST_CALL ||
           kind == AYNI_POLICY_REQUEST_MODEL;
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
                      const WorkModel &model, uint32_t task_index) {
    const auto &tasks = model.tasks;
    const work::Task &task = tasks.at(task_index);
    Verdict verdict{Verdict::Kind::progress, work::TaskChange::complete, AYNI_OK, AYNI_REASON_NONE};
    uint32_t step_count = model.accepted_step_count(task_index);
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
        if (intent.kind == AYNI_POLICY_REQUEST_CALL || intent.kind == AYNI_POLICY_REQUEST_MODEL) {
            if (intent_count != 1U || !catalog::CatalogId::is_valid(step_key_of(intent))) {
                return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT);
            }
            return {Verdict::Kind::call, work::TaskChange::complete, AYNI_OK, AYNI_REASON_NONE};
        }
        if (intent.kind == AYNI_POLICY_REPORT_PROGRESS) {
            const ayni_string_v1 key = step_key_of(intent);
            if (!catalog::CatalogId::is_valid(key)) {
                return hold(AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT);
            }
            if (model.step_taken(task_index, key) || commits_earlier(decisions, index, key)) {
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
    if (task.checkpoint_writer_version != 0U) {
        snapshot.checkpoint.strategy_id = {task.checkpoint_writer_id.data(),
                                           task.checkpoint_writer_id.size()};
        snapshot.checkpoint.strategy_version = task.checkpoint_writer_version;
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
// checkpoint its strategy, writer, left in the buffer, all in stamp.
void record(work::TaskTable &tasks, uint32_t task_index, const DecisionBuffer &decisions,
            uint32_t intent_count, const catalog::Strategy &writer, uint32_t checkpoint_size,
            foundation::TransitionStamp stamp) {
    for (uint32_t index = 0; index < intent_count; ++index) {
        const ayni_policy_intent_v1 &intent = decisions.at(index);
        if (intent.kind == AYNI_POLICY_REPORT_PROGRESS) {
            // Cannot fail: judge_intents checked each step.
            static_cast<void>(tasks.add_step(task_index, step_key_of(intent), stamp));
        }
    }
    tasks.set_checkpoint(task_index, writer.id, writer.version,
                         decisions.checkpoint(checkpoint_size), stamp);
}

// Publish one already judged decision. Call acceptance is the same transition
// as the strategy checkpoint, and any invariant failure is reported to Kernel.
DecisionResult publish_decision(WorkModel &model, DecisionBuffer &decisions, uint32_t index,
                                const catalog::Strategy &strategy,
                                const ayni_policy_output_v1 &output, Verdict verdict,
                                CallRuntime *calls, OwnerClock clock) {
    work::WorkTables tables = model.tables();
    if (verdict.kind == Verdict::Kind::end && work::task_end_exhausts(tables, index, verdict.end)) {
        verdict = hold(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVISION_EXHAUSTED);
    }
    const foundation::TransitionStamp stamp = model.transitions.next();
    switch (verdict.kind) {
    case Verdict::Kind::end:
        record(model.tasks, index, decisions, output.intent_count, strategy, output.checkpoint_size,
               stamp);
        work::end_task(tables, index, verdict.end, stamp);
        // An end frees a place in the task table and may end goals.
        model.due_work_changed = true;
        break;
    case Verdict::Kind::progress:
        record(model.tasks, index, decisions, output.intent_count, strategy, output.checkpoint_size,
               stamp);
        model.tasks.progress(index, stamp);
        break;
    case Verdict::Kind::call:
        record(model.tasks, index, decisions, output.intent_count, strategy, output.checkpoint_size,
               stamp);
        if (!calls->accept(index, stamp, clock.now_us(clock.ctx))) {
            return DecisionResult::internal;
        }
        break;
    case Verdict::Kind::hold:
        if (verdict.hold_reason == AYNI_REASON_TASK_MODEL_CALL_LIMIT) {
            model.tasks.hold_model_calls(index, stamp);
        } else {
            model.tasks.hold(index, verdict.hold_status, verdict.hold_reason, stamp);
        }
        // A hold keeps the goal from deriving (work::derivation_blocker).
        model.due_work_changed = true;
        break;
    }
    return DecisionResult::applied;
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

DecisionResult decide_next(WorkModel &model, DecisionBuffer &decisions, uint64_t ready_before,
                           uint64_t now_us, uint64_t durable_seq, OwnerClock clock,
                           CallRuntime *calls) {
    uint32_t index = 0;
    const DecisionAdmission admission{model, durable_seq};
    if (!model.tasks.next_ready(ready_before, index, {&admission, admission_ready})) {
        return DecisionResult::none;
    }
    const work::Task &task = model.tasks.at(index);
    const auto authority = model.check_task_authority(index, clock.now_us(clock.ctx), durable_seq);
    if (authority.status != AYNI_OK) {
        if (authority.status == AYNI_BUSY) {
            return DecisionResult::none;
        }
        model.pause_goal_authority(task.goal_index);
        return DecisionResult::applied;
    }
    const uint32_t strategy_index =
        model.goal_types.at(model.goals.at(task.goal_index).goal_type_index).strategy_index;
    const catalog::Strategy &strategy = model.strategies.at(strategy_index);

    ayni_task_snapshot_v1 snapshot = snapshot_of(model, decisions, index, now_us);
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
    if (calls != nullptr) {
        calls->begin_decision(index, snapshot, output);
    }
    model.calling_out = true;
    const ayni_status_t status = strategy.decide(strategy.ctx, &snapshot, &output);
    model.calling_out = false;
    if (calls != nullptr) {
        calls->end_decision();
    }

    // A non-reentrant callback can still run past a Grant deadline. Discard all
    // returned progress/checkpoint output before committing if authority expired.
    if (model.check_task_authority(index, clock.now_us(clock.ctx), durable_seq).status != AYNI_OK) {
        model.pause_goal_authority(task.goal_index);
        return DecisionResult::applied;
    }
    Verdict verdict = judge_output(status, output.intent_count, output.checkpoint_size, strategy);
    if (verdict.kind != Verdict::Kind::hold) {
        verdict = judge_intents(decisions, output.intent_count, model, index);
    }
    if (verdict.kind == Verdict::Kind::call) {
        const auto check =
            calls == nullptr
                ? authority::GrantCheck{AYNI_UNSUPPORTED, AYNI_REASON_INTENT_UNSUPPORTED}
                : calls->check_proposal(index, decisions.at(0), clock.now_us(clock.ctx),
                                        durable_seq);
        if (check.status != AYNI_OK) {
            verdict = hold(check.status, check.reason);
        }
    }
    return publish_decision(model, decisions, index, strategy, output, verdict, calls, clock);
}

bool awaits_decision(const WorkModel &model, uint64_t durable_seq) {
    const DecisionAdmission admission{model, durable_seq};
    return model.tasks.has_ready({&admission, admission_ready});
}

} // namespace ayni::kernel
