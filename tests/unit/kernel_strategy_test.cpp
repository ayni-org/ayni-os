// Strategies through the C ABI (ADR-0022): registration and the strategy id
// of goal types, their resolution by ayni_start, the decision ayni_step asks
// of each READY task, what a decision does to the task and its goal, the
// holds that refuse a decision, and the order and budget of decisions. Runs
// on the simulation Host.

#include "kernel/kernel_state.h"
#include "kernel_node.h"
#include "work_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace ayni::test;

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();
constexpr uint64_t no_time = std::numeric_limits<uint64_t>::max();
constexpr uint64_t second = AYNI_TRIGGER_PERIOD_MIN_US;

uint64_t create(WorkNode &work, const ayni_goal_create_v1 &body) {
    const Submitted created = work.create(body);
    EXPECT_EQ(created.status, AYNI_OK);
    return created.result.object.id;
}

uint64_t once_goal(WorkNode &work, uint64_t parent_goal_id = 0, uint64_t agent_id = agent_a) {
    return create(work, WorkNode::create_body(work.instance(agent_id), parent_goal_id, "reminder",
                                              1, {nullptr, 0}, AYNI_TRIGGER_ONCE));
}

// A goal that only organizes its children.
uint64_t folder_goal(WorkNode &work, uint64_t parent_goal_id) {
    return create(work, WorkNode::create_body(work.instance(agent_a), parent_goal_id, "folder", 1,
                                              {nullptr, 0}, AYNI_TRIGGER_NONE));
}

uint64_t every_second_goal(WorkNode &work, uint64_t agent_id = agent_a) {
    ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_id), 0, "timer", 1,
                                                     {nullptr, 0}, AYNI_TRIGGER_INTERVAL);
    body.trigger_period_us = second;
    return create(work, body);
}

uint64_t task_of(WorkNode &work, uint64_t goal_id) {
    return work.goal(goal_id).last_task_id;
}

uint32_t step_items(WorkNode &work, uint32_t max_items = unbounded_items) {
    return work.node().step(max_items).items_done;
}

ayni_policy_intent_v1 intent(ayni_policy_intent_kind_t kind, uint32_t expected_revision) {
    return policy_intent(kind, expected_revision);
}

ayni_status_t fail(const ayni_task_snapshot_v1 &snapshot, ayni_policy_output_v1 &output) {
    return report(AYNI_POLICY_REPORT_FAILURE, snapshot, output);
}

// White-box: puts the task at the largest revision.
void exhaust_task(WorkNode &work, uint64_t task_id) {
    const ayni::work::TaskTable &tasks = work.kernel()->work.tasks;
    uint32_t index = 0;
    ASSERT_TRUE(tasks.find(task_id, index));
    const_cast<ayni::work::Task &>(tasks.at(index)).revision = max_revision;
}

// White-box: the goal's mutable record.
ayni::work::Goal &raw_goal(WorkNode &work, uint64_t goal_id) {
    const ayni::work::GoalTable &goals = work.kernel()->work.goals;
    uint32_t index = 0;
    EXPECT_TRUE(goals.find(goal_id, index));
    return const_cast<ayni::work::Goal &>(goals.at(index));
}

// --- Registration ----------------------------------------------------------

TEST(WorkStrategy, registration_checks_its_arguments) {
    Node node;
    node.config().strategy_capacity = 2;
    node.config().policy_intent_capacity = 3;
    node.create();
    ayni_kernel_t *kernel = node.kernel();
    ScriptedStrategy strategy;
    // task_checkpoint_max_bytes is 8 (kernel_node.h).
    const ayni_strategy_v1 valid =
        strategy_desc("plan", 1, &ScriptedStrategy::decide, &strategy, 3, 8);

    EXPECT_EQ(ayni_register_strategy(nullptr, &valid), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_register_strategy(kernel, nullptr), AYNI_INVALID_ARGUMENT);
    ayni_strategy_v1 desc = valid;
    desc.struct_size -= 1U;
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_INVALID_ARGUMENT);
    desc.struct_size = sizeof(desc) + 8U;
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_UNSUPPORTED_VERSION);
    desc = valid;
    desc.intent_size -= 1U;
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_INVALID_ARGUMENT);
    desc.intent_size = 0U;
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_INVALID_ARGUMENT);
    desc.intent_size = sizeof(ayni_policy_intent_v1) + 4U;
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_UNSUPPORTED_VERSION);
    for (int field = 0; field < 9; ++field) {
        desc = valid;
        switch (field) {
        case 0:
            desc.version = 0;
            break;
        case 1:
            desc.strategy_id = text("");
            break;
        case 2:
            desc.strategy_id = text("Plan");
            break;
        case 3:
            desc.strategy_id = text("abcdefghijklmnopqrstuvwxyz0123456");
            break;
        case 4:
            desc.decide = nullptr;
            break;
        case 5:
            desc.max_intents_per_call = 0;
            break;
        case 6:
            desc.max_intents_per_call = 4; // above policy_intent_capacity
            break;
        case 7:
            desc.max_checkpoint_bytes = 9; // above task_checkpoint_max_bytes
            break;
        default:
            desc.reserved = 1;
            break;
        }
        EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_INVALID_ARGUMENT) << field;
    }

    EXPECT_EQ(ayni_register_strategy(kernel, &valid), AYNI_OK);
    // C-POLICY-010: one version of an id at a time.
    desc = valid;
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_INVALID_ARGUMENT);
    desc.version = 2;
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_INVALID_ARGUMENT);
    desc = strategy_desc("watch", 1, &ScriptedStrategy::decide, nullptr);
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_OK);
    desc = strategy_desc("other", 1, &ScriptedStrategy::decide, nullptr);
    EXPECT_EQ(ayni_register_strategy(kernel, &desc), AYNI_CAPACITY_EXCEEDED);
    // A duplicate is an argument error even once the registry is full.
    EXPECT_EQ(ayni_register_strategy(kernel, &valid), AYNI_INVALID_ARGUMENT);
}

// ADR-0022: a goal type names the strategy that decides its goals' tasks,
// and only goals that may only have no trigger have none to decide.
TEST(WorkStrategy, goal_type_names_a_strategy_exactly_when_its_goals_may_have_tasks) {
    Node node;
    node.create();
    ayni_kernel_t *kernel = node.kernel();

    ayni_goal_type_v1 type = goal_type_desc("reminder", 1);
    for (const ayni_string_v1 strategy_id :
         {ayni_string_v1{nullptr, 0}, text("Plan"), ayni_string_v1{nullptr, 4}}) {
        type.strategy_id = strategy_id;
        EXPECT_EQ(ayni_register_goal_type(kernel, &type), AYNI_INVALID_ARGUMENT);
    }
    // NONE among other kinds still lets goals have tasks.
    type =
        goal_type_desc("reminder", 1, nullptr, nullptr,
                       AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE) | AYNI_TRIGGER_BIT(AYNI_TRIGGER_ONCE));
    type.strategy_id = text("");
    EXPECT_EQ(ayni_register_goal_type(kernel, &type), AYNI_INVALID_ARGUMENT);
    type.strategy_id = text("plan");
    EXPECT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);

    ayni_goal_type_v1 folder =
        goal_type_desc("folder", 1, nullptr, nullptr, AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE));
    folder.strategy_id = text("plan");
    EXPECT_EQ(ayni_register_goal_type(kernel, &folder), AYNI_INVALID_ARGUMENT);
    folder.strategy_id = {"plan", 0}; // empty, whatever it points at
    EXPECT_EQ(ayni_register_goal_type(kernel, &folder), AYNI_OK);
}

// C-LIFE-003: ayni_start resolves every goal type's strategy, whatever the
// order of registration, and stays in CONFIGURING when one is missing.
TEST(WorkStrategy, start_resolves_strategy_ids_and_waits_for_a_missing_one) {
    Node node;
    node.config().strategy_capacity = 2;
    node.create();
    ayni_kernel_t *kernel = node.kernel();
    ScriptedStrategy strategy;
    const ayni_strategy_v1 spare = strategy_desc("spare", 1, &ScriptedStrategy::decide, &strategy);
    ASSERT_EQ(ayni_register_strategy(kernel, &spare), AYNI_OK);
    ayni_goal_type_v1 type = goal_type_desc("reminder", 1);
    type.strategy_id = text("plan");
    ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);

    EXPECT_EQ(ayni_start(kernel), AYNI_NOT_FOUND);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_CONFIGURING);
    const ayni_strategy_v1 plan = strategy_desc("plan", 1, &ScriptedStrategy::decide, &strategy);
    ASSERT_EQ(ayni_register_strategy(kernel, &plan), AYNI_OK);
    EXPECT_EQ(ayni_start(kernel), AYNI_OK);
    EXPECT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RECOVERING);
}

// --- Decisions -------------------------------------------------------------

// ADR-0022: the step after the one that made a task READY has its strategy
// decide it, with a snapshot of the task and an output in kernel memory.
// One completion ends the task and its ONCE goal in one transition.
TEST(WorkDecision, task_is_decided_in_the_step_after_it_became_ready) {
    WorkNode work;
    work.run();
    work.node().sim().advance_us(1000);
    const uint64_t created_at = work.node().sim().now_us();
    const uint8_t parameters[] = {4, 2};
    const uint64_t goal_id =
        create(work, WorkNode::create_body(work.instance(agent_a), 0, "reminder", 1,
                                           {parameters, 2}, AYNI_TRIGGER_ONCE));
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_TRUE(work.strategy().seen.empty());
    const uint64_t task_id = task_of(work, goal_id);
    const ayni_task_info_v1 ready = work.task(task_id);

    work.node().sim().advance_us(500);
    const uint64_t decided_at = work.node().sim().now_us();
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    ASSERT_EQ(work.strategy().seen.size(), 1U);
    const Seen &seen = work.strategy().seen[0];
    const ayni_task_snapshot_v1 &snapshot = seen.snapshot;
    EXPECT_EQ(snapshot.struct_size, sizeof(snapshot));
    EXPECT_EQ(snapshot.revision, 1U);
    EXPECT_EQ(snapshot.task_id, task_id);
    EXPECT_EQ(snapshot.goal_id, goal_id);
    EXPECT_EQ(snapshot.owner_instance_id, work.instance(agent_a));
    EXPECT_EQ(snapshot.due_us, created_at);
    EXPECT_EQ(snapshot.now_us, decided_at);
    EXPECT_EQ(snapshot.occurrence, 1U);
    EXPECT_EQ(snapshot.occurrence_count, 1U);
    EXPECT_EQ(snapshot.goal_type_version, 1U);
    EXPECT_EQ(snapshot.reserved, 0U);
    EXPECT_EQ(seen.goal_type_id, "reminder");
    EXPECT_EQ(seen.parameters, (std::vector<uint8_t>{4, 2}));
    const ayni_policy_output_v1 &output = work.strategy().outputs[0];
    EXPECT_EQ(output.struct_size, sizeof(output));
    EXPECT_EQ(output.intent_capacity, 1U); // the strategy's, not policy_intent_capacity
    EXPECT_EQ(output.intent_count, 0U);
    EXPECT_EQ(output.reserved, 0U);
    EXPECT_NE(output.intents, nullptr);
    // ADR-0024: no steps yet, and a strategy without a private checkpoint
    // gets an empty one.
    EXPECT_EQ(snapshot.checkpoint.strategy_id.size, 0U);
    EXPECT_EQ(snapshot.checkpoint.strategy_version, 0U);
    EXPECT_EQ(snapshot.checkpoint.reserved, 0U);
    EXPECT_EQ(snapshot.checkpoint.state.size, 0U);
    EXPECT_EQ(snapshot.step_count, 0U);
    EXPECT_EQ(snapshot.step_capacity, 4U); // task_step_capacity
    EXPECT_NE(snapshot.steps, nullptr);
    EXPECT_EQ(output.checkpoint_capacity, 0U);
    EXPECT_EQ(output.checkpoint_size, 0U);

    const ayni_task_info_v1 task = work.task(task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(task.revision, 2U);
    EXPECT_EQ(task.cancel_revision, 0U);
    EXPECT_EQ(task.hold_status, AYNI_OK);
    EXPECT_EQ(task.hold_reason, AYNI_REASON_NONE);
    EXPECT_EQ(task.step_count, 0U);
    EXPECT_EQ(task.reserved, 0U);
    EXPECT_GT(task.commit_seq, ready.commit_seq);
    EXPECT_GT(task.commit_seq, work.node().info().durable_seq);
    const ayni_goal_info_v1 goal = work.goal(goal_id);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(goal.revision, 2U);
    EXPECT_EQ(goal.cancel_revision, 0U);
    EXPECT_EQ(goal.commit_seq, task.commit_seq);

    EXPECT_EQ(step_items(work), 0U);
    EXPECT_EQ(work.strategy().seen.size(), 1U);
}

// A failed task fails a one-shot goal; a recurring goal goes on to its next
// occurrence, as after a completed task.
TEST(WorkDecision, failure_fails_a_one_shot_goal_and_a_recurring_goal_goes_on) {
    WorkNode work;
    work.run();
    work.strategy().answer = fail;
    const uint64_t once = once_goal(work);
    const uint64_t recurring = every_second_goal(work);
    EXPECT_EQ(step_items(work), 2U);
    const uint64_t first_task = task_of(work, recurring);
    EXPECT_EQ(step_items(work), 2U);

    EXPECT_EQ(work.task(task_of(work, once)).lifecycle, AYNI_TASK_FAILED);
    EXPECT_EQ(work.goal(once).lifecycle, AYNI_GOAL_FAILED);
    EXPECT_EQ(work.goal(once).commit_seq, work.task(task_of(work, once)).commit_seq);
    EXPECT_EQ(work.task(first_task).lifecycle, AYNI_TASK_FAILED);
    EXPECT_EQ(work.goal(recurring).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(recurring).revision, 1U);

    work.node().sim().advance_us(second);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_NE(task_of(work, recurring), first_task);
    EXPECT_EQ(work.task(task_of(work, recurring)).occurrence, 2U);
}

// A goal ends with the last of its tasks, once no occurrence is to come.
TEST(WorkDecision, goal_ends_with_the_last_of_its_tasks) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = every_second_goal(work);
    EXPECT_EQ(step_items(work, 1), 1U);
    work.node().sim().advance_us(second);
    EXPECT_EQ(step_items(work, 1), 1U); // the second task; the first waits
    const uint64_t first_task = work.task(task_of(work, goal_id)).task_id - 1U;
    ASSERT_EQ(work.task(first_task).lifecycle, AYNI_TASK_READY);
    // White-box: the second occurrence was the last.
    raw_goal(work, goal_id).cursor.next_due_us = no_time;

    EXPECT_EQ(step_items(work), 2U);
    const ayni_task_info_v1 first = work.task(first_task);
    const ayni_task_info_v1 last = work.task(task_of(work, goal_id));
    EXPECT_EQ(first.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(last.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_LT(first.commit_seq, last.commit_seq);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(work.goal(goal_id).commit_seq, last.commit_seq);
}

// ADR-0022: a goal's end, by a completed or a failed task, cancels its live
// descendants and their tasks in the same transition, as GOAL_CANCEL would;
// a task it cancels is not decided.
TEST(WorkDecision, goal_end_cancels_its_live_descendants_and_their_tasks) {
    for (const bool completes : {true, false}) {
        SCOPED_TRACE(completes ? "complete" : "fail");
        WorkNode work;
        if (!completes) {
            work.strategy().answer = fail;
        }
        work.run();
        const uint64_t root = once_goal(work);
        const uint64_t child = once_goal(work, root);
        const uint64_t grandchild = folder_goal(work, child);
        const uint64_t done_child = folder_goal(work, root);
        const uint64_t other = once_goal(work);
        EXPECT_EQ(step_items(work), 3U);
        ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, done_child).status, AYNI_OK);
        const ayni_goal_info_v1 done_before = work.goal(done_child);
        const uint32_t child_revision = work.goal(child).revision;
        const ayni_task_info_v1 child_task = work.task(task_of(work, child));

        EXPECT_EQ(step_items(work), 2U); // the root's task and the other's
        ASSERT_EQ(work.strategy().seen.size(), 2U);
        EXPECT_EQ(work.strategy().seen[0].snapshot.goal_id, root);
        EXPECT_EQ(work.strategy().seen[1].snapshot.goal_id, other);
        const ayni_goal_info_v1 ended = work.goal(root);
        EXPECT_EQ(ended.lifecycle, completes ? AYNI_GOAL_SATISFIED : AYNI_GOAL_FAILED);
        for (const uint64_t goal_id : {child, grandchild}) {
            const ayni_goal_info_v1 goal = work.goal(goal_id);
            EXPECT_EQ(goal.lifecycle, AYNI_GOAL_CANCELLED) << goal_id;
            EXPECT_EQ(goal.cancel_revision, goal.revision) << goal_id;
            EXPECT_EQ(goal.commit_seq, ended.commit_seq) << goal_id;
        }
        EXPECT_EQ(work.goal(child).revision, child_revision + 1U);
        const ayni_task_info_v1 cancelled = work.task(child_task.task_id);
        EXPECT_EQ(cancelled.lifecycle, AYNI_TASK_CANCELLED);
        EXPECT_EQ(cancelled.revision, child_task.revision + 1U);
        EXPECT_EQ(cancelled.cancel_revision, cancelled.revision);
        EXPECT_EQ(cancelled.commit_seq, ended.commit_seq);
        EXPECT_EQ(work.goal(done_child).revision, done_before.revision);
        EXPECT_EQ(work.goal(done_child).commit_seq, done_before.commit_seq);
        EXPECT_EQ(work.goal(other).lifecycle, ended.lifecycle);
    }
}

// --- Holds -----------------------------------------------------------------

struct HoldCase {
    const char *name;
    ScriptedStrategy::Answer answer;
    ayni_status_t status;
    ayni_reason_t reason;
};

// ayni_decide_fn: any decision that does not end the task holds it PAUSED
// with the status and reason of the first check it fails, in the documented
// order, and the strategy is not asked again.
TEST(WorkDecision, refused_decisions_hold_the_task_with_their_reason) {
    using Snapshot = const ayni_task_snapshot_v1 &;
    using Output = ayni_policy_output_v1 &;
    const HoldCase cases[] = {
        {"decide fails", [](Snapshot, Output) { return AYNI_INTERNAL; }, AYNI_INTERNAL,
         AYNI_REASON_NONE},
        {"no status",
         [](Snapshot s, Output o) {
             report(AYNI_POLICY_REPORT_COMPLETION, s, o);
             return static_cast<ayni_status_t>(1);
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_NONE},
        {"status before intents",
         [](Snapshot s, Output o) {
             report(AYNI_POLICY_REPORT_COMPLETION, s, o);
             return AYNI_INVALID_ARGUMENT;
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_NONE},
        {"needs more room", [](Snapshot, Output) { return AYNI_CAPACITY_EXCEEDED; },
         AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT},
        {"count past the capacity",
         [](Snapshot, Output o) {
             o.intent_count = 3;
             return AYNI_OK;
         },
         AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT},
        {"no intent", [](Snapshot, Output) { return AYNI_OK; }, AYNI_INVALID_ARGUMENT,
         AYNI_REASON_POLICY_NO_PROGRESS},
        {"other struct size",
         [](Snapshot s, Output o) {
             o.intents[0] = intent(AYNI_POLICY_REPORT_COMPLETION, s.revision);
             o.intents[0].struct_size -= 1U;
             o.intent_count = 1;
             return AYNI_OK;
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
        {"size before kind",
         [](Snapshot s, Output o) {
             o.intents[0] = intent(99, s.revision);
             o.intents[0].struct_size = 1;
             o.intent_count = 1;
             return AYNI_OK;
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
        {"kind not implemented",
         [](Snapshot s, Output o) {
             o.intents[0] = intent(1, s.revision); // a draft kind
             o.intent_count = 1;
             return AYNI_OK;
         },
         AYNI_UNSUPPORTED, AYNI_REASON_INTENT_UNSUPPORTED},
        {"kind before revision",
         [](Snapshot s, Output o) {
             o.intents[0] = intent(0, s.revision + 1U);
             o.intent_count = 1;
             return AYNI_OK;
         },
         AYNI_UNSUPPORTED, AYNI_REASON_INTENT_UNSUPPORTED},
        {"stale revision",
         [](Snapshot s, Output o) {
             o.intents[0] = intent(AYNI_POLICY_REPORT_COMPLETION, s.revision - 1U);
             o.intent_count = 1;
             return AYNI_OK;
         },
         AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION},
        {"two ends",
         [](Snapshot s, Output o) {
             o.intents[0] = intent(AYNI_POLICY_REPORT_COMPLETION, s.revision);
             o.intents[1] = intent(AYNI_POLICY_REPORT_FAILURE, s.revision);
             o.intent_count = 2;
             return AYNI_OK;
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
        {"end then a bad intent",
         [](Snapshot s, Output o) {
             o.intents[0] = intent(AYNI_POLICY_REPORT_FAILURE, s.revision);
             o.intents[1] = intent(AYNI_POLICY_REPORT_COMPLETION, s.revision - 1U);
             o.intent_count = 2;
             return AYNI_OK;
         },
         AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION},
    };
    for (const HoldCase &c : cases) {
        SCOPED_TRACE(c.name);
        WorkNode work;
        work.max_intents_per_call = 2;
        work.run();
        work.strategy().answer = c.answer;
        const uint64_t goal_id = once_goal(work);
        ASSERT_EQ(step_items(work), 1U);
        const ayni_task_info_v1 ready = work.task(task_of(work, goal_id));

        const ayni_step_report_v1 report = work.node().step();
        EXPECT_EQ(report.items_done, 1U);
        EXPECT_EQ(report.more_ready, 0U);
        const ayni_task_info_v1 held = work.task(ready.task_id);
        EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(held.hold_status, c.status);
        EXPECT_EQ(held.hold_reason, c.reason);
        EXPECT_EQ(held.revision, ready.revision + 1U);
        EXPECT_GT(held.commit_seq, ready.commit_seq);
        EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(work.goal(goal_id).revision, 1U);
        // A held task is not decided again.
        EXPECT_EQ(step_items(work), 0U);
        EXPECT_EQ(work.strategy().seen.size(), 1U);
    }
}

// Each decision starts from cleared intents, and the kernel reads back only
// its own array, wherever the strategy points the output.
TEST(WorkDecision, strategy_starts_from_cleared_intents_that_only_the_kernel_reads) {
    WorkNode work;
    work.max_intents_per_call = 2;
    work.run();
    ayni_policy_intent_v1 own[2] = {};
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &output) {
        for (uint32_t index = 0; index < output.intent_capacity; ++index) {
            EXPECT_EQ(output.intents[index].struct_size, 0U) << index;
            EXPECT_EQ(output.intents[index].kind, 0U) << index;
            EXPECT_EQ(output.intents[index].expected_revision, 0U) << index;
            EXPECT_EQ(output.intents[index].step_key_size, 0U) << index;
            const char *key = output.intents[index].step_key;
            EXPECT_TRUE(std::all_of(key, key + AYNI_CATALOG_ID_MAX_BYTES, [](char byte) {
                return byte == '\0';
            })) << index;
        }
        if (work.strategy().seen.size() == 1U) {
            // A step key on an end is refused, and left for the next decision
            // to find cleared.
            output.intents[0] =
                policy_intent(AYNI_POLICY_REPORT_COMPLETION, snapshot.revision, "left");
            output.intents[1] = intent(AYNI_POLICY_REPORT_COMPLETION, snapshot.revision);
            output.intent_count = 2;
            return AYNI_OK;
        }
        own[0] = intent(AYNI_POLICY_REPORT_COMPLETION, snapshot.revision);
        output.intents = own;
        output.intent_capacity = 2;
        output.intent_count = 1;
        return AYNI_OK;
    };
    const uint64_t first = once_goal(work);
    const uint64_t second_goal = once_goal(work);
    ASSERT_EQ(step_items(work), 2U);
    EXPECT_EQ(step_items(work), 2U);
    ASSERT_EQ(work.strategy().seen.size(), 2U);
    EXPECT_EQ(work.task(task_of(work, first)).hold_reason, AYNI_REASON_POLICY_INTENT);
    const ayni_task_info_v1 held = work.task(task_of(work, second_goal));
    EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(held.hold_status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(held.hold_reason, AYNI_REASON_POLICY_INTENT);
}

// ADR-0022: no goal or instance change resumes a held task; cancel and
// expiry still end it, and the hold stays readable.
TEST(WorkDecision, held_task_stays_paused_until_it_ends) {
    WorkNode work;
    work.run();
    work.strategy().answer = cannot_decide;
    const uint64_t goal_id = once_goal(work);
    ayni_goal_create_v1 expiring_body = WorkNode::create_body(work.instance(agent_b), 0, "reminder",
                                                              1, {nullptr, 0}, AYNI_TRIGGER_ONCE);
    expiring_body.expiry_us = 5U * second;
    const uint64_t expiring = create(work, expiring_body);
    ASSERT_EQ(step_items(work), 2U);
    ASSERT_EQ(step_items(work), 2U);
    const uint64_t task_id = task_of(work, goal_id);
    const ayni_task_info_v1 held = work.task(task_id);
    ASSERT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    ASSERT_EQ(held.hold_status, AYNI_UNSUPPORTED);

    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_a).status, AYNI_OK);
    EXPECT_EQ(step_items(work), 0U);
    const ayni_task_info_v1 still = work.task(task_id);
    EXPECT_EQ(still.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(still.revision, held.revision);
    EXPECT_EQ(still.commit_seq, held.commit_seq);
    EXPECT_EQ(work.strategy().seen.size(), 2U);

    ASSERT_EQ(work.cancel_task(task_id).status, AYNI_OK);
    const ayni_task_info_v1 cancelled = work.task(task_id);
    EXPECT_EQ(cancelled.lifecycle, AYNI_TASK_CANCELLED);
    EXPECT_EQ(cancelled.hold_status, AYNI_UNSUPPORTED);
    EXPECT_EQ(cancelled.hold_reason, AYNI_REASON_NONE);
    // It was the one-shot goal's last task (ADR-0023).
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_CANCELLED);

    work.node().sim().advance_us(5U * second);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.goal(expiring).lifecycle, AYNI_GOAL_EXPIRED);
    EXPECT_EQ(work.task(task_of(work, expiring)).lifecycle, AYNI_TASK_EXPIRED);
}

// ADR-0022: a goal with a held task derives no other: its occurrences wait
// with TASK_HELD and are skipped once their wait ends, while the goals of
// other agents go on through the task table they share. Cancelling the held
// task lets the goal derive again.
TEST(WorkDecision, held_task_keeps_its_goal_from_deriving_until_it_ends) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.node().config().task_capacity = 2;
    work.run();
    const uint64_t start_us = work.node().sim().now_us();
    const uint64_t refused = every_second_goal(work, agent_a);
    const uint64_t other = every_second_goal(work, agent_b);
    work.strategy().answer = [&](const ayni_task_snapshot_v1 &snapshot,
                                 ayni_policy_output_v1 &output) {
        return snapshot.goal_id == refused
                   ? AYNI_UNSUPPORTED
                   : report(AYNI_POLICY_REPORT_COMPLETION, snapshot, output);
    };
    ASSERT_EQ(step_items(work), 2U);
    ASSERT_EQ(step_items(work), 2U);
    const uint64_t held = task_of(work, refused);
    ASSERT_EQ(work.task(held).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(work.goal(refused).wait_reason, AYNI_REASON_TASK_HELD);
    EXPECT_EQ(work.goal(other).wait_reason, AYNI_REASON_NONE);

    // The other goal takes the place of its own ended task; the refused one
    // waits, though it is first in a tie.
    work.node().sim().advance_us(second);
    const uint64_t before = task_of(work, other);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_NE(task_of(work, other), before);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.task(task_of(work, other)).lifecycle, AYNI_TASK_COMPLETED);
    ayni_goal_info_v1 waiting = work.goal(refused);
    EXPECT_EQ(waiting.last_task_id, held);
    EXPECT_EQ(waiting.occurrences, 1U);
    EXPECT_EQ(waiting.next_due_us, start_us + second);
    EXPECT_EQ(waiting.wait_reason, AYNI_REASON_TASK_HELD);
    EXPECT_EQ(waiting.occurrences_skipped, 0U);

    // The wait of the second occurrence, due at one second, ends at three,
    // when the fourth falls due.
    work.node().sim().advance_us(2U * second);
    EXPECT_EQ(step_items(work), 2U);
    const ayni_goal_info_v1 skipped = work.goal(refused);
    EXPECT_EQ(skipped.last_task_id, held);
    EXPECT_EQ(skipped.occurrences, 4U);
    EXPECT_EQ(skipped.occurrences_skipped, 3U);
    EXPECT_EQ(skipped.skip_reason, AYNI_REASON_TASK_HELD);
    EXPECT_EQ(skipped.next_due_us, start_us + 4U * second);
    EXPECT_EQ(work.task(task_of(work, other)).occurrence_count, 2U);
    EXPECT_EQ(work.task(held).lifecycle, AYNI_TASK_PAUSED);

    ASSERT_EQ(work.cancel_task(held).status, AYNI_OK);
    EXPECT_EQ(work.goal(refused).wait_reason, AYNI_REASON_NONE);
    // The refused goal derives its fifth occurrence, and the other goal's
    // task from the step before is decided.
    work.node().sim().advance_us(second);
    EXPECT_EQ(step_items(work), 2U);
    const ayni_goal_info_v1 derived = work.goal(refused);
    EXPECT_NE(derived.last_task_id, held);
    EXPECT_EQ(derived.occurrences, 5U);
    EXPECT_EQ(derived.occurrences_skipped, 3U);
    EXPECT_EQ(work.task(derived.last_task_id).lifecycle, AYNI_TASK_READY);
}

// A goal with a held task waits even while the task table has room: the
// step that holds the task reports the end of the goal's wait as the next
// deadline, and the occurrences are skipped then.
TEST(WorkDecision, held_task_keeps_its_goal_waiting_with_room_in_the_table) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.run();
    const uint64_t start_us = work.node().sim().now_us();
    const uint64_t goal_id = every_second_goal(work);
    work.strategy().answer = cannot_decide;
    ASSERT_EQ(work.node().step().next_deadline_us, start_us + second);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, start_us + 3U * second);
    const uint64_t held = task_of(work, goal_id);

    work.node().sim().advance_us(3U * second);
    EXPECT_EQ(step_items(work), 1U);
    const ayni_goal_info_v1 skipped = work.goal(goal_id);
    EXPECT_EQ(skipped.last_task_id, held);
    EXPECT_EQ(skipped.occurrences_skipped, 3U);
    EXPECT_EQ(skipped.skip_reason, AYNI_REASON_TASK_HELD);
}

// ADR-0022: a held task concerns its own goal, so TASK_HELD comes before a
// full table in the goal's wait and skip reasons, while another goal waits
// for the table. Waits that end together skip the first created first.
TEST(WorkDecision, held_reason_comes_before_a_full_table) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t refused = every_second_goal(work);
    ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_b), 0, "timer", 1,
                                                     {nullptr, 0}, AYNI_TRIGGER_DELAY);
    body.trigger_delay_us = second;
    const uint64_t other = create(work, body);
    work.strategy().answer = cannot_decide;
    ASSERT_EQ(step_items(work), 1U);
    ASSERT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.goal(refused).wait_reason, AYNI_REASON_TASK_HELD);
    EXPECT_EQ(work.goal(other).wait_reason, AYNI_REASON_TASK_CAPACITY);

    work.node().sim().advance_us(3U * second);
    ASSERT_EQ(step_items(work, 1), 1U);
    EXPECT_EQ(work.goal(refused).occurrences_skipped, 3U);
    EXPECT_EQ(work.goal(other).occurrences_skipped, 0U);
    ASSERT_EQ(step_items(work), 1U);
    const ayni_goal_info_v1 refused_info = work.goal(refused);
    EXPECT_EQ(refused_info.occurrences_skipped, 3U);
    EXPECT_EQ(refused_info.skip_reason, AYNI_REASON_TASK_HELD);
    const ayni_goal_info_v1 other_info = work.goal(other);
    EXPECT_EQ(other_info.occurrences_skipped, 1U);
    EXPECT_EQ(other_info.skip_reason, AYNI_REASON_TASK_CAPACITY);
}

// ADR-0022: a goal can hold several tasks, those derived before the first
// hold, and waits until the last of them ends; an occurrence that waited
// then derives at the next step. The goal lies past the first 32, which a
// search gathers in one word.
TEST(WorkDecision, goal_waits_until_its_last_held_task_ends) {
    WorkNode work;
    work.node().config().goal_capacity = 40;
    work.run();
    for (int folder = 0; folder < 32; ++folder) {
        folder_goal(work, 0);
    }
    const uint64_t refused = every_second_goal(work);
    work.strategy().answer = cannot_decide;
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t first = task_of(work, refused);
    // The second occurrence derives before the first task is decided.
    work.node().sim().advance_us(second);
    ASSERT_EQ(step_items(work), 2U);
    const uint64_t second_task = task_of(work, refused);
    ASSERT_NE(second_task, first);
    ASSERT_EQ(step_items(work), 1U);
    ASSERT_EQ(work.task(first).lifecycle, AYNI_TASK_PAUSED);
    ASSERT_EQ(work.task(second_task).lifecycle, AYNI_TASK_PAUSED);
    work.node().sim().advance_us(second);
    EXPECT_EQ(step_items(work), 0U);
    EXPECT_EQ(work.goal(refused).wait_reason, AYNI_REASON_TASK_HELD);

    ASSERT_EQ(work.cancel_task(first).status, AYNI_OK);
    EXPECT_EQ(work.goal(refused).wait_reason, AYNI_REASON_TASK_HELD);
    EXPECT_EQ(step_items(work), 0U);
    ASSERT_EQ(work.cancel_task(second_task).status, AYNI_OK);
    EXPECT_EQ(work.goal(refused).wait_reason, AYNI_REASON_NONE);
    EXPECT_EQ(step_items(work), 1U);
    const ayni_task_info_v1 derived = work.task(task_of(work, refused));
    EXPECT_EQ(derived.occurrence, 3U);
    EXPECT_EQ(derived.occurrence_count, 1U);
}

// C-COMMIT-012: an end that would raise a revision past the largest, the
// task's goal's, a descendant's or a descendant's task's, holds the task
// instead; an end that leaves the goal as it is still applies.
TEST(WorkDecision, end_that_would_exhaust_a_revision_holds_the_task) {
    for (int exhausted = 0; exhausted < 3; ++exhausted) {
        SCOPED_TRACE(exhausted);
        WorkNode work;
        work.run();
        const uint64_t root = once_goal(work);
        const uint64_t child = exhausted == 2 ? once_goal(work, root) : folder_goal(work, root);
        ASSERT_EQ(step_items(work), exhausted == 2 ? 2U : 1U);
        switch (exhausted) {
        case 0:
            raw_goal(work, root).revision = max_revision;
            break;
        case 1:
            raw_goal(work, child).revision = max_revision;
            break;
        default:
            exhaust_task(work, task_of(work, child));
            break;
        }
        // A task at the largest revision is never decided itself.
        const ayni_step_report_v1 report = work.node().step();
        EXPECT_EQ(report.items_done, 1U);
        EXPECT_EQ(report.more_ready, 0U);
        EXPECT_EQ(work.strategy().seen.size(), 1U);
        const ayni_task_info_v1 held = work.task(task_of(work, root));
        EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(held.hold_status, AYNI_CAPACITY_EXCEEDED);
        EXPECT_EQ(held.hold_reason, AYNI_REASON_REVISION_EXHAUSTED);
        EXPECT_EQ(work.goal(root).lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(work.goal(child).lifecycle, AYNI_GOAL_ACTIVE);
    }

    WorkNode work;
    work.run();
    const uint64_t recurring = every_second_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    raw_goal(work, recurring).revision = max_revision;
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.task(task_of(work, recurring)).lifecycle, AYNI_TASK_COMPLETED);
}

// --- Order, budget and states ------------------------------------------------

// C-LIMIT-011, ADR-0022: each decision is one item, and tasks are decided in
// the order they became READY.
TEST(WorkDecision, tasks_are_decided_in_the_order_they_became_ready) {
    WorkNode work;
    work.run();
    const uint64_t goals[] = {once_goal(work), once_goal(work), once_goal(work)};
    ASSERT_EQ(step_items(work), 3U);
    // Paused and resumed, the first is READY again only since then.
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goals[0]).status, AYNI_OK);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goals[0]).status, AYNI_OK);

    ayni_step_report_v1 report = work.node().step(1);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    report = work.node().step(1);
    EXPECT_EQ(report.more_ready, 1U);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    const std::vector<Seen> &seen = work.strategy().seen;
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_EQ(seen[0].snapshot.goal_id, goals[1]);
    EXPECT_EQ(seen[1].snapshot.goal_id, goals[2]);
    EXPECT_EQ(seen[2].snapshot.goal_id, goals[0]);
}

// Tasks that became READY in one transition are decided in the order of
// their ids, not of their places in the table.
TEST(WorkDecision, tasks_ready_in_one_transition_are_decided_by_id) {
    WorkNode work;
    work.node().config().task_capacity = 3;
    work.run();
    const uint64_t done = once_goal(work);
    const uint64_t older = once_goal(work);
    const uint64_t newer = once_goal(work);
    ASSERT_EQ(step_items(work), 3U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, older).status, AYNI_OK);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, newer).status, AYNI_OK);
    ASSERT_EQ(step_items(work), 1U);
    ASSERT_EQ(work.goal(done).lifecycle, AYNI_GOAL_SATISFIED);
    // White-box: the next task takes the place of the one that ended, and
    // the table keeps it by moving its last task, the newer, to the front.
    const uint64_t last = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const ayni::work::TaskTable &tasks = work.kernel()->work.tasks;
    uint32_t older_index = 0;
    uint32_t newer_index = 0;
    ASSERT_TRUE(tasks.find(task_of(work, older), older_index));
    ASSERT_TRUE(tasks.find(task_of(work, newer), newer_index));
    ASSERT_LT(newer_index, older_index);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, older).status, AYNI_OK);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, newer).status, AYNI_OK);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_a).status, AYNI_OK);
    ASSERT_EQ(work.task(task_of(work, older)).commit_seq,
              work.task(task_of(work, newer)).commit_seq);

    EXPECT_EQ(step_items(work), 3U);
    const std::vector<Seen> &seen = work.strategy().seen;
    ASSERT_EQ(seen.size(), 4U);
    EXPECT_EQ(seen[1].snapshot.goal_id, older);
    EXPECT_EQ(seen[2].snapshot.goal_id, newer);
    EXPECT_EQ(seen[3].snapshot.goal_id, last);
}

TEST(WorkDecision, step_stops_deciding_once_its_time_budget_is_spent) {
    WorkNode work;
    work.run();
    once_goal(work);
    once_goal(work);
    ASSERT_EQ(step_items(work), 2U);
    work.node().sim().advance_per_read_us(5);
    const ayni_step_report_v1 report = work.node().step(unbounded_items, 5);
    work.node().sim().advance_per_read_us(0);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_EQ(step_items(work), 1U);
}

// A task that ends frees its place, so an occurrence waiting for one is due
// as soon as the decision applies.
TEST(WorkDecision, decision_that_ends_a_task_makes_room_for_a_waiting_occurrence) {
    WorkNode work;
    work.node().config().task_capacity = 1;
    work.run();
    const uint64_t first = once_goal(work);
    const uint64_t waiting = once_goal(work);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(work.goal(waiting).wait_reason, AYNI_REASON_TASK_CAPACITY);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_EQ(work.goal(first).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(step_items(work), 1U);
    EXPECT_NE(task_of(work, waiting), 0U);
}

// lifecycles.md: DRAINING takes no new work, and a decision is new work.
TEST(WorkDecision, only_running_decides) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    const ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_TRUE(work.strategy().seen.empty());
    // White-box: STOPPED answers no query, but the tables are still there.
    const ayni::kernel::WorkModel &model = work.kernel()->work;
    uint32_t index = 0;
    ASSERT_TRUE(model.tasks.find(task_id, index));
    EXPECT_EQ(model.tasks.at(index).lifecycle, AYNI_TASK_READY);
}

// C-POLICY-001: while a strategy decides, the kernel refuses each owner-thread
// function that changes it (contracts/domain/lifecycles.md), so the task the
// strategy saw is the one it decides.
TEST(WorkDecision, strategy_cannot_change_the_kernel_while_it_decides) {
    WorkNode work;
    work.run();
    ayni_kernel_t *kernel = work.kernel();
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_a));
    const ayni_intent_v1 nested = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    std::vector<ayni_status_t> inside;
    work.strategy().during = [&] {
        inside.push_back(ayni_submit_intent(kernel, &nested, nullptr, nullptr));
        const ayni_session_desc_v1 session = session_desc();
        uint64_t session_id = 0;
        inside.push_back(ayni_open_session(kernel, &session, &session_id));
        inside.push_back(ayni_close_session(kernel, work.session()));
        inside.push_back(ayni_step(kernel, 1, 1, nullptr));
        inside.push_back(ayni_begin_shutdown(kernel));
        inside.push_back(ayni_start(kernel));
        inside.push_back(ayni_destroy(kernel));
        const ayni_principal_desc_v1 principal = owner_desc(owner_id + 1U);
        inside.push_back(ayni_register_principal(kernel, &principal));
        const ayni_agent_desc_v1 agent = agent_desc(agent_b + 1U);
        inside.push_back(ayni_register_agent(kernel, &agent));
        const ayni_goal_type_v1 type = goal_type_desc("other", 1);
        inside.push_back(ayni_register_goal_type(kernel, &type));
        const ayni_strategy_v1 strategy =
            strategy_desc("other", 1, &ScriptedStrategy::decide, &work.strategy());
        inside.push_back(ayni_register_strategy(kernel, &strategy));
    };

    EXPECT_EQ(step_items(work), 1U);
    EXPECT_EQ(inside, std::vector<ayni_status_t>(11, AYNI_INVALID_STATE));
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_SATISFIED);
    // The session is still open, and the guard is gone once the step returns.
    EXPECT_EQ(work.create(body).status, AYNI_OK);
}

} // namespace
