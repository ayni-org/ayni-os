// Steps and private checkpoints through the C ABI (ADR-0024): decisions that
// report progress commit steps and leave their task READY for a later step,
// the private checkpoint a strategy returns comes back in the task's next
// snapshot, refused decisions commit nothing, the steps bound how often a
// task is decided, and pauses and new tasks keep or start the checkpoints.
// Runs on the simulation Host.

#include "kernel/kernel_state.h"
#include "kernel_node.h"
#include "work_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace ayni::test;

using Snapshot = const ayni_task_snapshot_v1 &;
using Output = ayni_policy_output_v1 &;

constexpr uint32_t max_revision = std::numeric_limits<uint32_t>::max();

uint64_t once_goal(WorkNode &work, uint64_t agent_id = agent_a) {
    const Submitted created = work.create(WorkNode::create_body(
        work.instance(agent_id), 0, "reminder", 1, {nullptr, 0}, AYNI_TRIGGER_ONCE));
    EXPECT_EQ(created.status, AYNI_OK);
    return created.result.object.id;
}

uint64_t task_of(WorkNode &work, uint64_t goal_id) {
    return work.goal(goal_id).last_task_id;
}

uint32_t step_items(WorkNode &work, uint32_t max_items = unbounded_items) {
    return work.node().step(max_items).items_done;
}

// Writes the intents into the output; the strategy returns AYNI_OK.
ayni_status_t decide_with(Output output, std::initializer_list<ayni_policy_intent_v1> intents) {
    uint32_t count = 0;
    for (const ayni_policy_intent_v1 &intent : intents) {
        output.intents[count++] = intent;
    }
    output.intent_count = count;
    return AYNI_OK;
}

void set_checkpoint(Output output, std::initializer_list<uint8_t> state) {
    uint32_t size = 0;
    for (const uint8_t byte : state) {
        output.checkpoint[size++] = byte;
    }
    output.checkpoint_size = size;
}

ayni_policy_intent_v1 complete(Snapshot snapshot) {
    return policy_intent(AYNI_POLICY_REPORT_COMPLETION, snapshot.revision);
}

// Answers the n-th decision (from 0) with answers[n], and completes the
// task after the last.
ScriptedStrategy::Answer in_turn(WorkNode &work, std::vector<ScriptedStrategy::Answer> answers) {
    return [&work, answers](Snapshot snapshot, Output output) {
        const std::size_t turn = work.strategy().seen.size() - 1U;
        if (turn < answers.size()) {
            return answers[turn](snapshot, output);
        }
        return decide_with(output, {complete(snapshot)});
    };
}

// White-box: sets the goal's revision.
void set_goal_revision(WorkNode &work, uint64_t goal_id, uint32_t revision) {
    const ayni::work::GoalTable &goals = work.kernel()->work.goals;
    uint32_t index = 0;
    ASSERT_TRUE(goals.find(goal_id, index));
    const_cast<ayni::work::Goal &>(goals.at(index)).revision = revision;
}

// White-box: the private checkpoint the kernel keeps for the task.
std::vector<uint8_t> kept_checkpoint(WorkNode &work, uint64_t task_id) {
    const ayni::work::TaskTable &tasks = work.kernel()->work.tasks;
    uint32_t index = 0;
    EXPECT_TRUE(tasks.find(task_id, index));
    return bytes_of(tasks.checkpoint(index));
}

// ADR-0024: a decision that reports progress commits its steps and leaves
// the task READY at its next revision; the task is decided again in the
// next step, which shows the steps in the order they were committed.
TEST(WorkCheckpoint, progress_commits_steps_and_leaves_the_task_ready) {
    WorkNode work;
    work.max_intents_per_call = 2;
    work.run();
    work.strategy().answer = in_turn(
        work,
        {[](Snapshot s, Output o) { return decide_with(o, {progress_intent("turn", s.revision)}); },
         [](Snapshot s, Output o) {
             return decide_with(o, {progress_intent("photo", s.revision),
                                    progress_intent("notify.owner", s.revision)});
         }});
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    const ayni_task_info_v1 ready = work.task(task_id);
    EXPECT_EQ(ready.step_count, 0U);
    EXPECT_EQ(ready.reserved, 0U);

    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U); // not decided twice in one step
    EXPECT_EQ(report.more_ready, 1U);
    const ayni_task_info_v1 first = work.task(task_id);
    EXPECT_EQ(first.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(first.revision, ready.revision + 1U);
    EXPECT_GT(first.commit_seq, ready.commit_seq);
    EXPECT_EQ(first.step_count, 1U);
    EXPECT_EQ(first.hold_status, AYNI_OK);
    EXPECT_EQ(first.hold_reason, AYNI_REASON_NONE);
    const ayni_goal_info_v1 goal = work.goal(goal_id);
    EXPECT_EQ(goal.lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(goal.revision, 1U);
    ASSERT_EQ(work.strategy().seen.size(), 1U);
    EXPECT_EQ(work.strategy().seen[0].snapshot.step_count, 0U);
    EXPECT_EQ(work.strategy().seen[0].snapshot.step_capacity, 4U); // task_step_capacity

    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    ASSERT_EQ(work.strategy().seen.size(), 2U);
    EXPECT_EQ(work.strategy().seen[1].snapshot.revision, first.revision);
    EXPECT_EQ(work.strategy().seen[1].steps, (std::vector<std::string>{"turn"}));
    EXPECT_EQ(work.task(task_id).step_count, 3U);
    EXPECT_EQ(work.task(task_id).revision, first.revision + 1U);

    EXPECT_EQ(step_items(work), 1U);
    ASSERT_EQ(work.strategy().seen.size(), 3U);
    EXPECT_EQ(work.strategy().seen[2].steps,
              (std::vector<std::string>{"turn", "photo", "notify.owner"}));
    const ayni_task_info_v1 done = work.task(task_id);
    EXPECT_EQ(done.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(done.step_count, 3U);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(step_items(work), 0U);
}

// The steps and private checkpoint of a decision that ends its task are
// committed in the same transition, whichever intent comes first.
TEST(WorkCheckpoint, decision_that_ends_its_task_commits_its_steps) {
    WorkNode work;
    work.max_intents_per_call = 2;
    work.max_checkpoint_bytes = 2;
    work.run();
    work.strategy().answer = [](Snapshot s, Output o) {
        set_checkpoint(o, {4});
        return decide_with(o, {complete(s), progress_intent("last", s.revision)});
    };
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ASSERT_EQ(step_items(work), 1U);

    const ayni_task_info_v1 done = work.task(task_id);
    EXPECT_EQ(done.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(done.revision, 2U);
    EXPECT_EQ(done.step_count, 1U);
    EXPECT_EQ(kept_checkpoint(work, task_id), (std::vector<uint8_t>{4}));
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_SATISFIED);
    EXPECT_EQ(work.goal(goal_id).commit_seq, done.commit_seq);
}

// Steps are told apart by their whole key: a key that is a prefix of
// another is a step of its own.
TEST(WorkCheckpoint, steps_that_share_a_prefix_are_distinct) {
    WorkNode work;
    work.max_intents_per_call = 2;
    work.run();
    work.strategy().answer =
        in_turn(work, {[](Snapshot s, Output o) {
                           return decide_with(o, {progress_intent("cd", s.revision),
                                                  progress_intent("c", s.revision)});
                       },
                       [](Snapshot s, Output o) {
                           return decide_with(o, {progress_intent("cde", s.revision), complete(s)});
                       }});
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ASSERT_EQ(step_items(work), 1U);
    ASSERT_EQ(step_items(work), 1U);

    const ayni_task_info_v1 done = work.task(task_id);
    EXPECT_EQ(done.lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(done.step_count, 3U);
    EXPECT_EQ(work.strategy().seen[1].steps, (std::vector<std::string>{"cd", "c"}));
}

// C-POLICY-007: the output starts with a copy of the task's private
// checkpoint, the rest of its capacity zeroed; what the decision leaves
// there comes back in the next snapshot, with the strategy's id and
// version. Before the first decision the checkpoint is empty and untagged.
TEST(WorkCheckpoint, private_checkpoint_comes_back_with_the_next_decision) {
    WorkNode work;
    work.max_checkpoint_bytes = 6;
    work.run();
    work.strategy().answer = in_turn(
        work,
        {[](Snapshot s, Output o) {
             set_checkpoint(o, {1, 2, 3});
             return decide_with(o, {progress_intent("a", s.revision)});
         },
         [](Snapshot s, Output o) { return decide_with(o, {progress_intent("b", s.revision)}); },
         [](Snapshot s, Output o) {
             o.checkpoint_size = 0;
             return decide_with(o, {progress_intent("c", s.revision)});
         }});
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    for (int decision = 0; decision < 4; ++decision) {
        ASSERT_EQ(step_items(work), 1U) << decision;
    }
    EXPECT_EQ(work.task(task_of(work, goal_id)).lifecycle, AYNI_TASK_COMPLETED);

    const std::vector<Seen> &seen = work.strategy().seen;
    const std::vector<ayni_policy_output_v1> &outputs = work.strategy().outputs;
    const std::vector<std::vector<uint8_t>> &given = work.strategy().output_checkpoints;
    ASSERT_EQ(seen.size(), 4U);
    EXPECT_EQ(seen[0].checkpoint_strategy_id, "");
    EXPECT_EQ(seen[0].snapshot.checkpoint.strategy_version, 0U);
    EXPECT_EQ(seen[0].snapshot.checkpoint.reserved, 0U);
    EXPECT_TRUE(seen[0].checkpoint.empty());
    EXPECT_EQ(outputs[0].checkpoint_capacity, 6U); // the strategy's, not the kernel's 8
    EXPECT_EQ(outputs[0].checkpoint_size, 0U);
    EXPECT_NE(outputs[0].checkpoint, nullptr);
    EXPECT_EQ(given[0], (std::vector<uint8_t>{0, 0, 0, 0, 0, 0}));

    for (std::size_t decision = 1; decision < 3; ++decision) {
        SCOPED_TRACE(decision);
        EXPECT_EQ(seen[decision].checkpoint_strategy_id, strategy_id);
        EXPECT_EQ(seen[decision].snapshot.checkpoint.strategy_version, 1U);
        EXPECT_EQ(seen[decision].checkpoint, (std::vector<uint8_t>{1, 2, 3}));
        EXPECT_EQ(outputs[decision].checkpoint_size, 3U);
        EXPECT_EQ(given[decision], (std::vector<uint8_t>{1, 2, 3, 0, 0, 0}));
    }
    // An empty checkpoint the strategy returned is still its own.
    EXPECT_EQ(seen[3].checkpoint_strategy_id, strategy_id);
    EXPECT_EQ(seen[3].snapshot.checkpoint.strategy_version, 1U);
    EXPECT_TRUE(seen[3].checkpoint.empty());
    EXPECT_EQ(given[3], (std::vector<uint8_t>(6, 0)));
}

// C-POLICY-003: the kernel reads back only its own checkpoint buffer, up to
// its own copy of the strategy's limit, and a task never sees what another
// task's decision left in the buffer.
TEST(WorkCheckpoint, kernel_reads_only_its_own_checkpoint_buffer) {
    WorkNode work;
    work.max_checkpoint_bytes = 4;
    work.run();
    uint8_t own[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    const uint64_t moved = once_goal(work);
    const uint64_t widened = once_goal(work);
    const uint64_t leftover = once_goal(work);
    work.strategy().answer = [&](Snapshot s, Output o) {
        if (s.goal_id == moved && s.step_count == 0U) {
            o.checkpoint = own;
            o.checkpoint_size = 2;
            return decide_with(o, {progress_intent("a", s.revision)});
        }
        if (s.goal_id == widened) {
            o.checkpoint_capacity = 8;
            o.checkpoint = own;
            o.checkpoint_size = 5;
            return decide_with(o, {progress_intent("a", s.revision)});
        }
        if (s.goal_id == leftover && s.step_count == 0U) {
            set_checkpoint(o, {7, 7, 7, 7});
            return decide_with(o, {progress_intent("a", s.revision)});
        }
        return decide_with(o, {complete(s)});
    };
    ASSERT_EQ(step_items(work), 3U);
    ASSERT_EQ(step_items(work), 3U);

    // The kernel kept the first two bytes of its own, zeroed buffer.
    EXPECT_EQ(kept_checkpoint(work, task_of(work, moved)), (std::vector<uint8_t>{0, 0}));
    const ayni_task_info_v1 held = work.task(task_of(work, widened));
    EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(held.hold_status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(held.hold_reason, AYNI_REASON_POLICY_OUTPUT_LIMIT);
    EXPECT_EQ(held.step_count, 0U);

    // The next decision of the first task starts from its own checkpoint,
    // not from the {7, 7, 7, 7} the last decision left in the buffer.
    ASSERT_EQ(step_items(work), 2U);
    const std::vector<std::vector<uint8_t>> &given = work.strategy().output_checkpoints;
    ASSERT_EQ(given.size(), 5U);
    EXPECT_EQ(work.strategy().seen[3].snapshot.goal_id, moved);
    EXPECT_EQ(given[3], (std::vector<uint8_t>{0, 0, 0, 0}));
    EXPECT_EQ(work.strategy().seen[4].checkpoint, (std::vector<uint8_t>{7, 7, 7, 7}));
    EXPECT_EQ(work.task(task_of(work, moved)).lifecycle, AYNI_TASK_COMPLETED);
}

// ayni_decide_fn: a refused decision holds the task with the first check it
// fails, and commits neither its steps nor its private checkpoint.
TEST(WorkCheckpoint, refused_decisions_commit_nothing_and_hold_with_their_reason) {
    struct HoldCase {
        const char *name;
        std::function<ayni_status_t(Snapshot, Output)> answer;
        ayni_status_t status;
        ayni_reason_t reason;
    };
    const auto too_long = [](Snapshot s) {
        ayni_policy_intent_v1 intent = progress_intent("c", s.revision);
        intent.step_key_size = AYNI_CATALOG_ID_MAX_BYTES + 1U;
        return intent;
    };
    const HoldCase cases[] = {
        {"checkpoint past the capacity",
         [](Snapshot s, Output o) {
             o.checkpoint_size = 5;
             return decide_with(o, {progress_intent("c", s.revision)});
         },
         AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT},
        {"checkpoint before no intent",
         [](Snapshot, Output o) {
             o.checkpoint_size = 5;
             return AYNI_OK;
         },
         AYNI_CAPACITY_EXCEEDED, AYNI_REASON_POLICY_OUTPUT_LIMIT},
        {"empty step key",
         [](Snapshot s, Output o) { return decide_with(o, {progress_intent("", s.revision)}); },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
        {"step key not a catalog id",
         [](Snapshot s, Output o) { return decide_with(o, {progress_intent("Turn", s.revision)}); },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
        {"step key too long",
         [too_long](Snapshot s, Output o) { return decide_with(o, {too_long(s)}); },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
        {"step key on an end",
         [](Snapshot s, Output o) {
             return decide_with(o, {policy_intent(AYNI_POLICY_REPORT_COMPLETION, s.revision, "c")});
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
        {"step the task committed",
         [](Snapshot s, Output o) { return decide_with(o, {progress_intent("a", s.revision)}); },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_STEP_TAKEN},
        {"step twice in the decision",
         [](Snapshot s, Output o) {
             return decide_with(
                 o, {progress_intent("c", s.revision), progress_intent("c", s.revision)});
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_STEP_TAKEN},
        {"steps past the capacity",
         [](Snapshot s, Output o) {
             return decide_with(
                 o, {progress_intent("c", s.revision), progress_intent("d", s.revision)});
         },
         AYNI_CAPACITY_EXCEEDED, AYNI_REASON_STEP_CAPACITY},
        {"taken before the capacity",
         [](Snapshot s, Output o) {
             return decide_with(
                 o, {progress_intent("c", s.revision), progress_intent("b", s.revision)});
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_STEP_TAKEN},
        {"invalid key after a step",
         [](Snapshot s, Output o) {
             return decide_with(
                 o, {progress_intent("c", s.revision), progress_intent("C", s.revision)});
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
        {"revision before the step key",
         [](Snapshot s, Output o) {
             return decide_with(o, {progress_intent("a", s.revision - 1U)});
         },
         AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION},
        {"revision before an end's step key",
         [](Snapshot s, Output o) {
             return decide_with(
                 o, {policy_intent(AYNI_POLICY_REPORT_COMPLETION, s.revision - 1U, "c")});
         },
         AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION},
        {"progress then a stale end",
         [](Snapshot s, Output o) {
             return decide_with(o, {progress_intent("c", s.revision),
                                    policy_intent(AYNI_POLICY_REPORT_COMPLETION, s.revision - 1U)});
         },
         AYNI_REVISION_CONFLICT, AYNI_REASON_TASK_REVISION},
        {"two ends",
         [](Snapshot s, Output o) {
             return decide_with(
                 o, {complete(s), policy_intent(AYNI_POLICY_REPORT_FAILURE, s.revision)});
         },
         AYNI_INVALID_ARGUMENT, AYNI_REASON_POLICY_INTENT},
    };
    for (const HoldCase &c : cases) {
        SCOPED_TRACE(c.name);
        WorkNode work;
        work.node().config().task_step_capacity = 3;
        work.max_intents_per_call = 2;
        work.max_checkpoint_bytes = 4;
        work.run();
        const std::function<ayni_status_t(Snapshot, Output)> refused = c.answer;
        work.strategy().answer =
            in_turn(work, {[](Snapshot s, Output o) {
                               set_checkpoint(o, {5, 6});
                               return decide_with(o, {progress_intent("a", s.revision),
                                                      progress_intent("b", s.revision)});
                           },
                           [refused](Snapshot s, Output o) {
                               o.checkpoint[0] = 8;
                               return refused(s, o);
                           }});
        const uint64_t goal_id = once_goal(work);
        ASSERT_EQ(step_items(work), 1U);
        const uint64_t task_id = task_of(work, goal_id);
        ASSERT_EQ(step_items(work), 1U);
        const ayni_task_info_v1 ready = work.task(task_id);
        ASSERT_EQ(ready.step_count, 2U);

        EXPECT_EQ(step_items(work), 1U);
        const ayni_task_info_v1 held = work.task(task_id);
        EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(held.hold_status, c.status);
        EXPECT_EQ(held.hold_reason, c.reason);
        EXPECT_EQ(held.revision, ready.revision + 1U);
        EXPECT_EQ(held.step_count, 2U);
        EXPECT_EQ(kept_checkpoint(work, task_id), (std::vector<uint8_t>{5, 6}));
        EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(step_items(work), 0U);
    }
}

// An end that would raise its goal's revision past the largest holds the
// task with REVISION_EXHAUSTED, and like every refused decision commits
// neither its steps nor its private checkpoint.
TEST(WorkCheckpoint, end_that_would_exhaust_a_revision_commits_nothing) {
    WorkNode work;
    work.max_intents_per_call = 2;
    work.max_checkpoint_bytes = 2;
    work.run();
    work.strategy().answer =
        in_turn(work, {[](Snapshot s, Output o) {
                           set_checkpoint(o, {5, 6});
                           return decide_with(o, {progress_intent("a", s.revision)});
                       },
                       [](Snapshot s, Output o) {
                           set_checkpoint(o, {8});
                           return decide_with(o, {progress_intent("b", s.revision), complete(s)});
                       }});
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ASSERT_EQ(step_items(work), 1U);
    set_goal_revision(work, goal_id, max_revision);
    ASSERT_EQ(step_items(work), 1U);

    const ayni_task_info_v1 held = work.task(task_id);
    EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(held.hold_status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(held.hold_reason, AYNI_REASON_REVISION_EXHAUSTED);
    EXPECT_EQ(held.step_count, 1U);
    EXPECT_EQ(kept_checkpoint(work, task_id), (std::vector<uint8_t>{5, 6}));
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
}

// The project owner's choice (ADR-0024): a decision that only changes the
// private checkpoint is no progress, so the task is held and keeps the
// checkpoint it had.
TEST(WorkCheckpoint, decision_that_only_changes_the_checkpoint_holds_the_task) {
    WorkNode work;
    work.max_checkpoint_bytes = 2;
    work.run();
    work.strategy().answer =
        in_turn(work, {[](Snapshot s, Output o) {
                           set_checkpoint(o, {1});
                           return decide_with(o, {progress_intent("a", s.revision)});
                       },
                       [](Snapshot, Output o) {
                           set_checkpoint(o, {2, 2});
                           return AYNI_OK;
                       }});
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ASSERT_EQ(step_items(work), 1U);
    ASSERT_EQ(step_items(work), 1U);

    const ayni_task_info_v1 held = work.task(task_id);
    EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(held.hold_status, AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(held.hold_reason, AYNI_REASON_POLICY_NO_PROGRESS);
    EXPECT_EQ(kept_checkpoint(work, task_id), (std::vector<uint8_t>{1}));
}

// Every decision that does not hold the task ends it or commits a step, so
// a strategy that only ever reports progress is held after
// task_step_capacity of them, at its task_step_capacity + 1st decision.
TEST(WorkCheckpoint, steps_bound_how_often_a_task_is_decided) {
    WorkNode work;
    work.node().config().task_step_capacity = 3;
    work.run();
    work.strategy().answer = [](Snapshot s, Output o) {
        return decide_with(o, {progress_intent("s" + std::to_string(s.step_count), s.revision)});
    };
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    uint32_t decisions = 0;
    while (step_items(work) > 0U) {
        ++decisions;
        ASSERT_LE(decisions, 10U);
    }

    EXPECT_EQ(decisions, 4U);
    EXPECT_EQ(work.strategy().seen.size(), 4U);
    EXPECT_EQ(work.strategy().seen[3].steps, (std::vector<std::string>{"s0", "s1", "s2"}));
    const ayni_task_info_v1 held = work.task(task_id);
    EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(held.hold_status, AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(held.hold_reason, AYNI_REASON_STEP_CAPACITY);
    EXPECT_EQ(held.step_count, 3U);
}

// A task left READY is decided after the tasks READY before it, as one
// that has just become READY (ADR-0022 decision 5).
TEST(WorkCheckpoint, progress_puts_the_task_behind_the_tasks_ready_before_it) {
    WorkNode work;
    work.run();
    work.strategy().answer = [](Snapshot s, Output o) {
        return decide_with(o, {progress_intent("s" + std::to_string(s.step_count), s.revision)});
    };
    const uint64_t first = once_goal(work);
    const uint64_t second = once_goal(work);
    ASSERT_EQ(step_items(work), 2U);
    for (int decision = 0; decision < 3; ++decision) {
        ASSERT_EQ(step_items(work, 1), 1U) << decision;
    }

    const std::vector<Seen> &seen = work.strategy().seen;
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_EQ(seen[0].snapshot.goal_id, first);
    EXPECT_EQ(seen[1].snapshot.goal_id, second);
    EXPECT_EQ(seen[2].snapshot.goal_id, first);
    EXPECT_EQ(seen[2].snapshot.step_count, 1U);
}

// C-LIFE-021, C-LIFE-023: a pause of the goal or of its owner keeps the
// task's steps and private checkpoint, and once resumed it is decided from
// them.
TEST(WorkCheckpoint, pause_keeps_the_steps_and_checkpoint_for_the_next_decision) {
    WorkNode work;
    work.max_checkpoint_bytes = 2;
    work.run();
    work.strategy().answer = [](Snapshot s, Output o) {
        if (s.step_count == 2U) {
            return decide_with(o, {complete(s)});
        }
        set_checkpoint(o, {static_cast<uint8_t>(10U + s.step_count)});
        return decide_with(o, {progress_intent("s" + std::to_string(s.step_count), s.revision)});
    };
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    ASSERT_EQ(step_items(work), 1U);

    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    const ayni_task_info_v1 paused = work.task(task_id);
    EXPECT_EQ(paused.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(paused.hold_status, AYNI_OK);
    EXPECT_EQ(paused.step_count, 1U);
    EXPECT_EQ(step_items(work), 0U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    ASSERT_EQ(step_items(work), 1U);

    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_a).status, AYNI_OK);
    EXPECT_EQ(step_items(work), 0U);
    ASSERT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_RESUME, agent_a).status, AYNI_OK);
    ASSERT_EQ(step_items(work), 1U);

    const std::vector<Seen> &seen = work.strategy().seen;
    ASSERT_EQ(seen.size(), 3U);
    EXPECT_EQ(seen[1].steps, (std::vector<std::string>{"s0"}));
    EXPECT_EQ(seen[1].checkpoint, (std::vector<uint8_t>{10}));
    EXPECT_EQ(seen[2].steps, (std::vector<std::string>{"s0", "s1"}));
    EXPECT_EQ(seen[2].checkpoint, (std::vector<uint8_t>{11}));
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_COMPLETED);
}

// A task that takes an ended task's place starts with no steps and no
// private checkpoint.
TEST(WorkCheckpoint, task_in_a_given_up_place_starts_without_steps_or_checkpoint) {
    WorkNode work;
    work.node().config().task_capacity = 1;
    work.max_intents_per_call = 2;
    work.max_checkpoint_bytes = 2;
    work.run();
    work.strategy().answer = [](Snapshot s, Output o) {
        set_checkpoint(o, {3, 4});
        return decide_with(o, {progress_intent("a", s.revision), complete(s)});
    };
    const uint64_t ended = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t ended_task = task_of(work, ended);
    ASSERT_EQ(step_items(work), 1U);
    ASSERT_EQ(work.task(ended_task).step_count, 1U);

    const uint64_t next = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    ayni_task_info_v1 gone{};
    gone.struct_size = sizeof(gone);
    EXPECT_EQ(ayni_get_task(work.kernel(), ended_task, &gone), AYNI_NOT_FOUND);
    EXPECT_EQ(work.task(task_of(work, next)).step_count, 0U);
    ASSERT_EQ(step_items(work), 1U);

    const std::vector<Seen> &seen = work.strategy().seen;
    ASSERT_EQ(seen.size(), 2U);
    EXPECT_EQ(seen[1].snapshot.goal_id, next);
    EXPECT_EQ(seen[1].snapshot.step_count, 0U);
    EXPECT_EQ(seen[1].checkpoint_strategy_id, "");
    EXPECT_EQ(seen[1].snapshot.checkpoint.strategy_version, 0U);
    EXPECT_TRUE(seen[1].checkpoint.empty());
    EXPECT_EQ(work.strategy().output_checkpoints[1], (std::vector<uint8_t>{0, 0}));
}

// ADR-0022 decision 11 holds for progress too: a decision may raise the
// task to the largest revision, and the task is then never decided again.
TEST(WorkCheckpoint, progress_to_the_largest_revision_leaves_the_task_undecided) {
    WorkNode work;
    work.run();
    work.strategy().answer = [](Snapshot s, Output o) {
        return decide_with(o, {progress_intent("a", s.revision)});
    };
    const uint64_t goal_id = once_goal(work);
    ASSERT_EQ(step_items(work), 1U);
    const uint64_t task_id = task_of(work, goal_id);
    {
        const ayni::work::TaskTable &tasks = work.kernel()->work.tasks;
        uint32_t index = 0;
        ASSERT_TRUE(tasks.find(task_id, index));
        const_cast<ayni::work::Task &>(tasks.at(index)).revision = max_revision - 1U;
    }

    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    const ayni_task_info_v1 task = work.task(task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(task.revision, max_revision);
    EXPECT_EQ(task.step_count, 1U);
    EXPECT_EQ(step_items(work), 0U);
    EXPECT_EQ(work.strategy().seen.size(), 1U);
}

} // namespace
