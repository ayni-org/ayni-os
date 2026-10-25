// The work log through the C ABI (ADR-0026): each transition's batch is
// appended in commit order, recovery replays the stored prefix and nothing
// after it, timers restart from the recovery clock (decision 6), what this
// boot's catalog cannot serve is held (decision 7), command ids return their
// first result across restarts (decision 9), a transition waits for room for
// its batch, a failed append restricts the kernel, and DRAINING stores what
// RUNNING committed. Runs on the simulation Host.

#include "durability/batch/batch_codec.h"
#include "kernel_node.h"
#include "kernel_test_probe.h"
#include "work/task/task_records.h"
#include "work_node.h"

#include <ayni/ayni.h>
#include <ayni/hosts/sim.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using ayni::kernel::KernelTestProbe;

namespace {

using namespace ayni::test;

using ayni::hosts::sim::CutAppend;
using ayni::hosts::sim::StorageFault;
using ayni::hosts::sim::StoragePort;
using ayni::hosts::sim::StoredBatch;

using Snapshot = const ayni_task_snapshot_v1 &;
using Output = ayni_policy_output_v1 &;

constexpr uint64_t no_time = UINT64_MAX;
constexpr uint64_t period_us = AYNI_TRIGGER_PERIOD_MIN_US;

// White-box: the commit_seq the next transition takes.
uint64_t next_commit_seq(WorkNode &work) {
    return KernelTestProbe::work(work.kernel()).transitions.next_commit_seq();
}

ayni_goal_create_v1 goal_body(WorkNode &work, ayni_trigger_kind_t trigger,
                              const char *type = "timer", uint64_t agent_id = agent_a,
                              uint64_t parent_goal_id = 0) {
    ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_id), parent_goal_id, type,
                                                     1, {nullptr, 0}, trigger);
    if (trigger == AYNI_TRIGGER_INTERVAL) {
        body.trigger_period_us = period_us;
    } else if (trigger == AYNI_TRIGGER_LOCAL_INPUT) {
        body.trigger_source = text("button");
    }
    return body;
}

uint64_t create(WorkNode &work, const ayni_goal_create_v1 &body) {
    const Submitted created = work.create(body);
    EXPECT_EQ(created.status, AYNI_OK);
    return created.result.object.id;
}

Submitted create_command(WorkNode &work, const ayni_goal_create_v1 &body, uint64_t command_id) {
    ayni_intent_v1 intent = work.intent(AYNI_INTENT_GOAL_CREATE, &body);
    intent.command_id = command_id;
    return work.submit(intent);
}

ayni_status_t post_input(WorkNode &work) {
    ayni_event_v1 event{};
    event.struct_size = sizeof(event);
    event.kind = AYNI_EVENT_LOCAL_INPUT;
    event.source = text("button");
    return ayni_post_event(work.kernel(), &event);
}

uint64_t task_of(WorkNode &work, uint64_t goal_id) {
    return work.goal(goal_id).last_task_id;
}

// Steps, storing every batch, until nothing is ready or in flight.
void run_until_idle(WorkNode &work) {
    for (int round = 0; round < 100; ++round) {
        const ayni_step_report_v1 report = work.node().step();
        if (work.node().sim().complete_storage() == 0U && report.more_ready == 0U) {
            // White-box: only advance the storage deadline, never a future
            // goal deadline merely because the application is idle.
            if (!KernelTestProbe::log(work.kernel()).idle() && report.next_deadline_us != no_time &&
                report.next_deadline_us > work.node().sim().now_us()) {
                work.node().sim().advance_us(report.next_deadline_us - work.node().sim().now_us());
                continue;
            }
            return;
        }
    }
    ADD_FAILURE() << "the kernel never went idle";
}

// Waits for the next write budget without completing the write. This lets
// scenarios place a power cut or inspect acceptance at a real I/O boundary.
ayni_step_report_v1 storage_step(WorkNode &work) {
    const uint64_t due = KernelTestProbe::log(work.kernel()).next_deadline();
    if (due != no_time && due > work.node().sim().now_us()) {
        work.node().sim().advance_us(due - work.node().sim().now_us());
    }
    return work.node().step();
}

// Reminder tasks report a step and a checkpoint, then complete; timer tasks
// cannot be decided, so the strategy holds them.
ayni_status_t mixed_answer(Snapshot snapshot, Output output) {
    if (string_of(snapshot.goal_type_id) == "timer") {
        return AYNI_UNSUPPORTED;
    }
    if (snapshot.step_count == 0U) {
        output.intents[0] = progress_intent("turn", snapshot.revision);
        output.intent_count = 1;
        output.checkpoint[0] = 7;
        output.checkpoint[1] = 8;
        output.checkpoint_size = 2;
        return AYNI_OK;
    }
    return report(AYNI_POLICY_REPORT_COMPLETION, snapshot, output);
}

// A work node whose strategy gives mixed_answer and keeps checkpoints.
void prepare(WorkNode &work) {
    work.max_checkpoint_bytes = 4;
}

// White-box persistent view for prefix atomicity: raw lifecycle, stored hold,
// original stamps and contents. Recovery compatibility/authorization overlays
// and derived wait reasons are deliberately excluded; the API gate is checked
// separately. Times that each boot anchors afresh (decision 6) are left out.
std::string describe(WorkNode &work) {
    const ayni::kernel::WorkModel &model = KernelTestProbe::work(work.kernel());
    std::ostringstream out;
    std::vector<uint64_t> ids;
    for (uint32_t index = 0; index < model.agents.size(); ++index) {
        ids.push_back(model.agents.at(index).agent_id);
    }
    std::sort(ids.begin(), ids.end());
    for (const uint64_t id : ids) {
        const ayni_agent_info_v1 agent = work.agent(id);
        out << "agent " << id << " lifecycle " << agent.lifecycle << " instance "
            << agent.instance_id << " seq " << agent.commit_seq << " revision " << agent.revision
            << " fault " << agent.fault_reason << '\n';
    }
    ids.clear();
    for (uint32_t index = 0; index < model.goals.size(); ++index) {
        ids.push_back(model.goals.at(index).goal_id);
    }
    std::sort(ids.begin(), ids.end());
    for (const uint64_t id : ids) {
        const ayni_goal_info_v1 goal = work.goal(id);
        uint32_t index = 0;
        EXPECT_TRUE(model.goals.find(id, index));
        const ayni::work::Goal &kept = model.goals.at(index);
        out << "goal " << id << " lifecycle " << kept.lifecycle << " parent " << goal.parent_goal_id
            << " owner " << goal.owner_instance_id << " origin " << goal.origin_principal_id << '/'
            << goal.origin << '/' << goal.origin_channel << '/' << goal.origin_binding_revision
            << " transition " << goal.transition_id << " seq " << goal.commit_seq << " revision "
            << goal.revision << '/' << goal.cancel_revision << " depth " << goal.depth << " type "
            << std::string(goal.goal_type_id, goal.goal_type_id_size) << '/'
            << goal.goal_type_version << " trigger " << goal.trigger << '/' << goal.trigger_delay_us
            << '/' << goal.trigger_period_us << '/'
            << std::string(goal.trigger_source, goal.trigger_source_size) << " occurrences "
            << goal.occurrences << '/' << goal.occurrences_skipped << '/' << goal.skip_reason
            << " last task " << goal.last_task_id << " expires "
            << (goal.expires_us == no_time ? "never" : "set") << " replaced "
            << model.goals.at(index).replaced_goal_id << " parameters";
        for (const uint8_t byte : bytes_of(model.goals.parameters(index))) {
            out << ' ' << static_cast<int>(byte);
        }
        out << '\n';
    }
    ids.clear();
    for (uint32_t index = 0; index < model.tasks.size(); ++index) {
        ids.push_back(model.tasks.at(index).task_id);
    }
    std::sort(ids.begin(), ids.end());
    for (const uint64_t id : ids) {
        const ayni_task_info_v1 task = work.task(id);
        uint32_t index = 0;
        EXPECT_TRUE(model.tasks.find(id, index));
        const ayni::work::Task &kept = model.tasks.at(index);
        out << "task " << id << " lifecycle " << kept.lifecycle << " goal " << task.goal_id
            << " owner " << task.owner_instance_id << " transition " << task.transition_id
            << " seq " << task.commit_seq << " revision " << task.revision << '/'
            << task.cancel_revision << " occurrence " << task.occurrence << '/'
            << task.occurrence_count << " hold " << kept.hold_status << '/' << kept.hold_reason
            << " replaced " << kept.replaced_task_id << " steps";
        for (uint32_t step = 0; step < task.step_count; ++step) {
            const ayni::catalog::CatalogId &key = model.tasks.step(index, step);
            out << ' ' << std::string(key.data(), key.size());
        }
        out << " checkpoint "
            << std::string(kept.checkpoint_writer_id.data(), kept.checkpoint_writer_id.size())
            << '/' << kept.checkpoint_writer_version;
        for (const uint8_t byte : bytes_of(model.tasks.checkpoint(index))) {
            out << ' ' << static_cast<int>(byte);
        }
        out << '\n';
    }
    return out.str();
}

// ADR-0038: compatible schema 2 work exposes its retained lifecycle and
// independent policy/user state, without introducing a recovery-only deny.
void expect_normal_authority(WorkNode &work) {
    const auto &model = KernelTestProbe::work(work.kernel());
    for (uint32_t index = 0; index < model.goals.size(); ++index) {
        const auto &stored = model.goals.at(index);
        const auto visible = work.goal(stored.goal_id);
        EXPECT_EQ(visible.lifecycle, stored.lifecycle);
        EXPECT_EQ(visible.hold_reason, stored.hold_reason);
    }
    for (uint32_t index = 0; index < model.tasks.size(); ++index) {
        const auto &stored = model.tasks.at(index);
        const auto visible = work.task(stored.task_id);
        EXPECT_EQ(visible.lifecycle, stored.lifecycle);
        EXPECT_EQ(visible.hold_status, stored.hold_status);
        EXPECT_EQ(visible.hold_reason, stored.hold_reason);
    }
}

// Records describe() after each transition, by its commit_seq. Each call
// it watches applies at most one transition.
class Views {
  public:
    explicit Views(WorkNode &work) : work_(work), next_(next_commit_seq(work)) {
        views_[next_ - 1U] = describe(work);
    }

    void record() {
        const uint64_t next = next_commit_seq(work_);
        if (next == next_) {
            return;
        }
        ASSERT_EQ(next, next_ + 1U) << "more than one transition in one call";
        views_[next_] = describe(work_);
        next_ = next;
    }

    // Steps one item at a time, storing every batch, until nothing is ready
    // or in flight.
    void run_until_idle() {
        for (int round = 0; round < 200; ++round) {
            const ayni_step_report_v1 report = work_.node().step(1);
            record();
            if (work_.node().sim().complete_storage() == 0U && report.more_ready == 0U) {
                if (!KernelTestProbe::log(work_.kernel()).idle() &&
                    report.next_deadline_us != no_time &&
                    report.next_deadline_us > work_.node().sim().now_us()) {
                    work_.node().sim().advance_us(report.next_deadline_us -
                                                  work_.node().sim().now_us());
                    continue;
                }
                return;
            }
        }
        ADD_FAILURE() << "the kernel never went idle";
    }

    void submit(const std::function<void()> &call) {
        call();
        record();
    }

    const std::map<uint64_t, std::string> &all() const { return views_; }

  private:
    WorkNode &work_;
    uint64_t next_;
    std::map<uint64_t, std::string> views_;
};

// Applies one of every kind of transition the log records, storing each
// batch, and returns the view after each.
std::map<uint64_t, std::string> run_scenario(WorkNode &work) {
    work.strategy().answer = mixed_answer;
    Views views(work);
    uint64_t container = 0;
    uint64_t interval = 0;
    uint64_t input = 0;
    views.submit([&] {
        ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_NONE, "reminder");
        const uint8_t parameters[] = {1, 2, 3};
        body.parameters = {parameters, sizeof(parameters)};
        body.expiry_us = 50U * period_us;
        const Submitted created = create_command(work, body, 9);
        EXPECT_EQ(created.status, AYNI_OK);
        container = created.result.object.id;
    });
    views.submit([&] { create(work, goal_body(work, AYNI_TRIGGER_ONCE, "reminder")); });
    views.submit([&] {
        interval =
            create(work, goal_body(work, AYNI_TRIGGER_INTERVAL, "timer", agent_a, container));
    });
    views.submit(
        [&] { input = create(work, goal_body(work, AYNI_TRIGGER_LOCAL_INPUT, "timer", agent_b)); });
    // Derives the ONCE goal's task and the interval's, has the strategy
    // hold the interval's, and gives the other a step and a checkpoint
    // before it completes, which satisfies its goal.
    views.run_until_idle();
    EXPECT_EQ(post_input(work), AYNI_OK);
    views.run_until_idle();
    views.submit([&] { EXPECT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, interval).status, AYNI_OK); });
    views.submit(
        [&] { EXPECT_EQ(work.change_agent(AYNI_INTENT_INSTANCE_PAUSE, agent_b).status, AYNI_OK); });
    views.submit([&] { EXPECT_EQ(work.cancel_task(task_of(work, input)).status, AYNI_OK); });
    work.node().sim().advance_us(2U * period_us);
    views.run_until_idle();
    views.submit(
        [&] { EXPECT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, container).status, AYNI_OK); });
    views.run_until_idle();
    EXPECT_EQ(work.node().sim().pending_storage(), 0U);
    return views.all();
}

// --- Prefixes ---------------------------------------------------------------

// C-STORE-001, C-COMMIT-004: a boot sees exactly the transitions of the
// batches stored before it, whatever prefix of the log power loss left:
// after recovering the first k batches the queries show what they showed
// right after transition k.
TEST(WorkRecovery, every_stored_prefix_recovers_the_state_after_its_last_batch) {
    WorkNode first;
    prepare(first);
    first.run();
    const std::map<uint64_t, std::string> views = run_scenario(first);
    const std::vector<StoredBatch> log = first.node().sim().log();
    ASSERT_EQ(log.size(), views.rbegin()->first);
    ASSERT_GE(log.size(), 12U);
    // The batches up to the instances' start: the agents are back RUNNING,
    // so recovery itself applies no transition.
    for (std::size_t kept = views.begin()->first; kept <= log.size(); ++kept) {
        SCOPED_TRACE(kept);
        WorkNode later;
        prepare(later);
        later.node().sim().log().assign(log.begin(), log.begin() + static_cast<long>(kept));
        later.run();
        EXPECT_EQ(later.node().info().durable_seq, kept + 2U); // its writer fence and boot batch
        EXPECT_EQ(describe(later), views.at(kept));
        expect_normal_authority(later);
    }
}

// An orderly restart stores every batch RUNNING committed, and the next
// boot shows the same work, held tasks and checkpoints included.
TEST(WorkRecovery, restart_after_shutdown_keeps_all_the_work) {
    WorkNode work;
    prepare(work);
    work.run();
    const std::map<uint64_t, std::string> views = run_scenario(work);
    const std::string before = describe(work);
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(describe(work), before);
    EXPECT_EQ(work.node().info().durable_seq, views.rbegin()->first + 2U);
}

// --- Power loss -------------------------------------------------------------

// C-STORE-002: power loss during an append leaves the batch lost, whole or
// torn. A lost or torn batch is not replayed and the next boot's batch takes
// its place; a whole one is, though the kernel never learned it was stored.
TEST(WorkRecovery, append_cut_by_power_loss_counts_only_when_whole) {
    for (const CutAppend cut : {CutAppend::lost, CutAppend::torn, CutAppend::stored}) {
        SCOPED_TRACE(static_cast<int>(cut));
        WorkNode work;
        work.run();
        const uint64_t kept = create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
        run_until_idle(work);
        const std::string stored = describe(work);
        const uint64_t cut_goal = create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
        const std::string cut_view = describe(work);
        storage_step(work); // the append is in flight
        ASSERT_EQ(work.node().sim().pending_storage(), 1U);

        work.node().power_cut_and_start(cut);
        work.node().recover();
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        EXPECT_EQ(describe(work), cut == CutAppend::stored ? cut_view : stored);
        EXPECT_EQ(work.goal(kept).lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(work.goal(kept).hold_reason, AYNI_REASON_NONE);
        ayni_goal_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_goal(work.kernel(), cut_goal, &info),
                  cut == CutAppend::stored ? AYNI_OK : AYNI_NOT_FOUND);
        // The old stored prefix is followed only by the new writer fence
        // and boot batch; authorization overlays add no transition.
        EXPECT_EQ(work.node().info().durable_seq, cut == CutAppend::stored ? 7U : 6U);
    }
}

// Batches still waiting in the journal ring are memory: power loss takes
// them, and the stored prefix ends before the first of them.
TEST(WorkRecovery, power_loss_takes_the_batches_never_appended) {
    WorkNode work;
    work.run();
    const std::string stored = describe(work);
    for (int goal = 0; goal < 3; ++goal) {
        create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
    }
    work.node().step(); // appends the first; the others wait behind it
    work.node().power_cut_and_start(CutAppend::lost);
    work.node().recover();
    EXPECT_EQ(describe(work), stored);
}

// --- Timers after a restart (decision 6) -------------------------------------

// ADR-0026 decision 6: each timer restarts from the recovery clock R with its
// full length: an INTERVAL with an occurrence taken after its period, one
// without after its delay, a DELAY after its delay, an expiry after its full
// duration, and a live task is due at R.
TEST(WorkRecovery, timers_restart_from_the_recovery_clock_with_their_full_length) {
    WorkNode work;
    work.run();
    work.strategy().answer = cannot_decide; // tasks stay live
    work.node().sim().advance_us(1000);
    ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_INTERVAL);
    body.trigger_delay_us = 300;
    const uint64_t taken = create(work, body);
    body.trigger_delay_us = 5U * period_us;
    const uint64_t not_taken = create(work, body);
    body = goal_body(work, AYNI_TRIGGER_DELAY);
    body.trigger_delay_us = 7U * period_us;
    const uint64_t delayed = create(work, body);
    body = goal_body(work, AYNI_TRIGGER_NONE, "folder");
    body.expiry_us = 9U * period_us;
    const uint64_t expiring = create(work, body);
    work.node().sim().advance_us(300);
    run_until_idle(work); // takes the first interval's occurrence
    const uint64_t task_id = task_of(work, taken);
    ASSERT_NE(task_id, 0U);
    ASSERT_EQ(work.goal(taken).occurrences, 1U);

    ASSERT_EQ(work.node().stop_and_destroy(), AYNI_OK);
    work.node().sim().advance_us(60U * period_us);
    work.node().create_and_start();
    work.node().recover();
    const uint64_t recovered_us = work.node().sim().now_us();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    // None of the 60 periods the power was off for is made up (C-LIMIT-007).
    EXPECT_EQ(work.goal(taken).occurrences, 1U);
    EXPECT_EQ(work.goal(taken).next_due_us, recovered_us + period_us);
    EXPECT_EQ(work.goal(not_taken).next_due_us, recovered_us + 5U * period_us);
    EXPECT_EQ(work.goal(delayed).next_due_us, recovered_us + 7U * period_us);
    EXPECT_EQ(work.goal(expiring).expires_us, recovered_us + 9U * period_us);
    EXPECT_EQ(work.task(task_id).due_us, recovered_us);
    // C-TIME-004 / ADR-0038: valid permanent authority rejoins the original
    // restart timer contract. The first interval remains blocked by its
    // policy-held Task, so the other interval supplies the next deadline.
    expect_normal_authority(work);
    EXPECT_EQ(work.node().step().next_deadline_us, recovered_us + 5U * period_us);
}

// ADR-0038: a new ingress instance may deliver input to normally recovered
// authorized work; no sequence is compared across boot incarnations.
TEST(WorkRecovery, recovered_input_goal_takes_new_boot_inputs) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = create(work, goal_body(work, AYNI_TRIGGER_LOCAL_INPUT));
    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(work.goal(goal_id).hold_reason, AYNI_REASON_NONE);
    EXPECT_EQ(work.goal(goal_id).next_due_us, no_time);
    ASSERT_EQ(post_input(work), AYNI_OK);
    work.node().step(1); // take input
    work.node().step(1); // derive task
    EXPECT_NE(task_of(work, goal_id), 0U);
    EXPECT_EQ(work.goal(goal_id).occurrences, 1U);
}

// --- What this boot cannot serve (decision 7) --------------------------------

struct Catalog {
    uint32_t reminder_version = 1; // 0: not registered
    bool timer_decided = true;     // false: the timer type has no strategy
    bool agent_b = true;
    uint32_t strategy_version = 1;
    const char *strategy_name = strategy_id;
};

// Registers catalog instead of the work node's own on each kernel created
// from now on.
void use_catalog(WorkNode &work, const Catalog &catalog) {
    work.node().on_create([&work, catalog](ayni_kernel_t *kernel) {
        const ayni_principal_desc_v1 owner = owner_desc();
        ASSERT_EQ(ayni_register_principal(kernel, &owner), AYNI_OK);
        const ayni_agent_desc_v1 a = agent_desc(agent_a);
        ASSERT_EQ(ayni_register_agent(kernel, &a), AYNI_OK);
        if (catalog.agent_b) {
            const ayni_agent_desc_v1 b = agent_desc(agent_b);
            ASSERT_EQ(ayni_register_agent(kernel, &b), AYNI_OK);
        }
        std::vector<ayni_goal_type_v1> types;
        if (catalog.reminder_version != 0U) {
            types.push_back(goal_type_desc("reminder", catalog.reminder_version));
        }
        types.push_back(goal_type_desc(
            "timer", 1, nullptr, nullptr,
            catalog.timer_decided ? any_trigger : AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE)));
        types.push_back(
            goal_type_desc("folder", 1, nullptr, nullptr, AYNI_TRIGGER_BIT(AYNI_TRIGGER_NONE)));
        for (auto type : types) {
            if (type.strategy_id.size != 0U) {
                type.strategy_id = text(catalog.strategy_name);
            }
            ASSERT_EQ(ayni_register_goal_type(kernel, &type), AYNI_OK);
        }
        const ayni_strategy_v1 strategy = strategy_desc(
            catalog.strategy_name, catalog.strategy_version, &ScriptedStrategy::decide,
            &work.strategy(), work.max_intents_per_call, work.max_checkpoint_bytes);
        ASSERT_EQ(ayni_register_strategy(kernel, &strategy), AYNI_OK);
    });
}

// Reminder tasks report a new step and a checkpoint at each decision, so
// they stay READY; timer tasks cannot be decided.
ayni_status_t progress_answer(Snapshot snapshot, Output output) {
    if (string_of(snapshot.goal_type_id) == "timer") {
        return AYNI_UNSUPPORTED;
    }
    output.intents[0] =
        progress_intent("turn" + std::to_string(snapshot.step_count), snapshot.revision);
    output.intent_count = 1;
    output.checkpoint[0] = 7;
    output.checkpoint[1] = static_cast<uint8_t>(snapshot.step_count);
    output.checkpoint_size = 2;
    return AYNI_OK;
}

// A reminder goal whose task wrote a checkpoint and waits READY for its
// next decision, and a timer goal whose task waits READY undecided.
struct HeldWork {
    uint64_t reminder;
    uint64_t reminder_task;
    uint64_t timer;
    uint64_t timer_task;
};

HeldWork live_tasks(WorkNode &work) {
    prepare(work);
    use_catalog(work, Catalog{});
    work.run();
    work.strategy().answer = progress_answer;
    HeldWork held{};
    held.reminder = create(work, goal_body(work, AYNI_TRIGGER_ONCE, "reminder"));
    work.node().step(); // derives its task
    work.node().step(); // its decision reports a step and a checkpoint
    held.reminder_task = task_of(work, held.reminder);
    EXPECT_EQ(work.task(held.reminder_task).step_count, 1U);
    held.timer = create(work, goal_body(work, AYNI_TRIGGER_ONCE, "timer"));
    work.node().step(); // derives its task, and decides the reminder's again
    held.timer_task = task_of(work, held.timer);
    EXPECT_EQ(work.task(held.timer_task).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(held.reminder_task).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(held.reminder_task).step_count, 2U);
    return held;
}

// Restarts with catalog and returns how many commit_seqs recovery took:
// the boot batch's, and the start transition's if it applied one.
uint64_t restart_with(WorkNode &work, const Catalog &catalog) {
    work.strategy().answer = cannot_decide;
    use_catalog(work, catalog);
    const uint64_t next = next_commit_seq(work);
    work.node().reboot();
    work.node().recover();
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    return next_commit_seq(work) - next;
}

// C-LIFE-024 / ADR-0038: missing types hold only incompatible work. Restoring
// the original catalog removes that volatile hold, preserving stored progress.
TEST(WorkRecovery, goal_type_this_boot_lacks_holds_its_goals_and_tasks) {
    for (const uint32_t version : {0U, 2U}) {
        SCOPED_TRACE(version);
        WorkNode work;
        const HeldWork held = live_tasks(work);
        const auto goal_before = work.goal(held.reminder);
        const auto task_before = work.task(held.reminder_task);
        Catalog catalog;
        catalog.reminder_version = version;
        restart_with(work, catalog);
        const auto goal = work.goal(held.reminder);
        const auto task = work.task(held.reminder_task);
        EXPECT_EQ(goal.hold_reason,
                  version == 0U ? AYNI_REASON_GOAL_TYPE_UNKNOWN : AYNI_REASON_GOAL_TYPE_VERSION);
        EXPECT_EQ(goal.revision, goal_before.revision);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(task.hold_reason, goal.hold_reason);
        EXPECT_EQ(task.revision, task_before.revision);
        EXPECT_EQ(task.commit_seq, task_before.commit_seq);
        EXPECT_EQ(work.goal(held.timer).hold_reason, AYNI_REASON_NONE);
        EXPECT_EQ(work.task(held.timer_task).lifecycle, AYNI_TASK_READY);
        const auto decisions = work.strategy().seen.size();
        work.node().step(1);
        ASSERT_EQ(work.strategy().seen.size(), decisions + 1U);
        EXPECT_EQ(work.strategy().seen.back().snapshot.task_id, held.timer_task);
        restart_with(work, Catalog{});
        EXPECT_EQ(work.goal(held.reminder).hold_reason, AYNI_REASON_NONE);
        EXPECT_EQ(work.task(held.reminder_task).lifecycle, AYNI_TASK_READY);
        EXPECT_EQ(work.task(held.reminder_task).hold_status, AYNI_OK);
        EXPECT_EQ(work.task(held.reminder_task).revision, task_before.revision);
        EXPECT_EQ(work.task(held.reminder_task).commit_seq, task_before.commit_seq);
    }
}

// ADR-0026 decision 7, C-POLICY-007, C-POLICY-010, C-LIFE-024: only the
// strategy that wrote a checkpoint, at that version, reads it; a task whose
// type has no strategy now cannot be decided, and a checkpoint larger than
// its strategy now keeps cannot be handed to it. Each task is held, PAUSED
// in memory only, with its steps, checkpoint and revision as stored, so a
// later compatible boot resumes from the original checkpoint (ADR-0038).
TEST(WorkRecovery, task_its_strategy_cannot_serve_is_held) {
    {
        SCOPED_TRACE("other strategy version");
        WorkNode work;
        const HeldWork held = live_tasks(work);
        const ayni_task_info_v1 before = work.task(held.reminder_task);
        Catalog catalog;
        catalog.strategy_version = 2;
        restart_with(work, catalog);
        const ayni_task_info_v1 task = work.task(held.reminder_task);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(task.revision, before.revision);
        EXPECT_EQ(task.step_count, before.step_count);
        EXPECT_EQ(task.hold_status, AYNI_UNSUPPORTED_VERSION);
        EXPECT_EQ(task.hold_reason, AYNI_REASON_STRATEGY_CHANGED);
        // The timer has no checkpoint and remains compatible.
        EXPECT_EQ(work.task(held.timer_task).lifecycle, AYNI_TASK_READY);
        EXPECT_EQ(work.task(held.timer_task).hold_reason, AYNI_REASON_NONE);

        restart_with(work, Catalog{});
        EXPECT_EQ(work.task(held.reminder_task).lifecycle, AYNI_TASK_READY);
        EXPECT_EQ(work.task(held.reminder_task).hold_status, AYNI_OK);
        EXPECT_EQ(work.task(held.reminder_task).hold_reason, AYNI_REASON_NONE);
        // White-box: compatibility recovery leaves private progress intact.

        uint32_t index = 0;
        ASSERT_TRUE(KernelTestProbe::work(work.kernel()).tasks.find(held.reminder_task, index));
        const auto &tasks = KernelTestProbe::work(work.kernel()).tasks;
        EXPECT_EQ(tasks.at(index).step_count, before.step_count);
        EXPECT_EQ(tasks.at(index).checkpoint_writer_version, 1U);
        const std::vector<uint8_t> resumed = {7, static_cast<uint8_t>(before.step_count - 1U)};
        EXPECT_EQ(bytes_of(tasks.checkpoint(index)), resumed);
        for (uint32_t step = 0; step < before.step_count; ++step) {
            const auto &key = tasks.step(index, step);
            EXPECT_EQ(std::string(key.data(), key.size()), "turn" + std::to_string(step));
        }
    }
    {
        SCOPED_TRACE("checkpoint larger than the strategy keeps");
        WorkNode work;
        const HeldWork held = live_tasks(work);
        work.max_checkpoint_bytes = 1;
        restart_with(work, Catalog{});
        const ayni_task_info_v1 task = work.task(held.reminder_task);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(task.hold_status, AYNI_CAPACITY_EXCEEDED);
        EXPECT_EQ(task.hold_reason, AYNI_REASON_POLICY_OUTPUT_LIMIT);
        EXPECT_EQ(work.task(held.timer_task).lifecycle, AYNI_TASK_READY);
        EXPECT_EQ(work.task(held.timer_task).hold_reason, AYNI_REASON_NONE);
    }
    {
        SCOPED_TRACE("type without a strategy");
        WorkNode work;
        const HeldWork held = live_tasks(work);
        Catalog catalog;
        catalog.timer_decided = false;
        restart_with(work, catalog);
        const ayni_task_info_v1 task = work.task(held.timer_task);
        EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
        EXPECT_EQ(task.hold_status, AYNI_UNSUPPORTED_VERSION);
        EXPECT_EQ(task.hold_reason, AYNI_REASON_STRATEGY_CHANGED);
        EXPECT_EQ(work.task(held.reminder_task).lifecycle, AYNI_TASK_READY);
        EXPECT_EQ(work.task(held.reminder_task).hold_reason, AYNI_REASON_NONE);
    }
}

// ADR-0026 decision 7: a held goal has no due work. Its pending occurrence
// and expiry neither derive a task nor end it, and give no deadline, so the
// log keeps them as they were for a boot that registers its type again; a
// goal whose type lost its strategy is held the same way, since nothing
// could decide its tasks. A held goal can still be cancelled, which ends
// the hold.
TEST(WorkRecovery, held_goal_has_no_due_work) {
    const struct {
        const char *name;
        const char *type;
        Catalog catalog;
        ayni_reason_t reason;
    } cases[] = {
        {"type unknown", "reminder", Catalog{0, true, true, 1}, AYNI_REASON_GOAL_TYPE_UNKNOWN},
        {"type without a strategy", "timer", Catalog{1, false, true, 1},
         AYNI_REASON_STRATEGY_CHANGED},
    };
    for (const auto &c : cases) {
        SCOPED_TRACE(c.name);
        WorkNode work;
        prepare(work);
        use_catalog(work, Catalog{});
        work.run();
        ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_DELAY, c.type);
        body.trigger_delay_us = 7U * period_us;
        body.expiry_us = 9U * period_us;
        const uint64_t goal_id = create(work, body);
        run_until_idle(work);

        restart_with(work, c.catalog);
        EXPECT_EQ(work.goal(goal_id).hold_reason, c.reason);
        work.node().sim().advance_us(10U * period_us);
        run_until_idle(work);
        EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(task_of(work, goal_id), 0U);
        EXPECT_EQ(work.node().step().next_deadline_us, no_time);

        restart_with(work, Catalog{});
        EXPECT_EQ(work.goal(goal_id).hold_reason, AYNI_REASON_NONE);
        EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
        const auto restored_at = work.node().sim().now_us();
        work.node().sim().advance_us(7U * period_us);
        run_until_idle(work);
        EXPECT_NE(task_of(work, goal_id), 0U);
        EXPECT_EQ(work.node().step().next_deadline_us, restored_at + 9U * period_us);
    }
    {
        SCOPED_TRACE("cancelled");
        WorkNode work;
        prepare(work);
        use_catalog(work, Catalog{});
        work.run();
        const uint64_t goal_id = create(work, goal_body(work, AYNI_TRIGGER_NONE, "reminder"));
        Catalog catalog;
        catalog.reminder_version = 0;
        restart_with(work, catalog);
        ASSERT_EQ(work.goal(goal_id).hold_reason, AYNI_REASON_GOAL_TYPE_UNKNOWN);
        ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, goal_id).status, AYNI_OK);
        EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_CANCELLED);
        EXPECT_EQ(work.goal(goal_id).hold_reason, AYNI_REASON_NONE);
    }
}

// ADR-0026 decision 7, C-LIFE-020: the instance of an agent this boot does
// not register keeps its goals and tasks, which no other instance takes; it
// is FAULTED with AGENT_UNREGISTERED in memory, without a revision, and its
// live Tasks follow the existing startup pause/resume transition. Returning
// the Agent removes its fault and permits normally authorized continuation.
TEST(WorkRecovery, instance_of_an_agent_no_longer_registered_is_faulted) {
    WorkNode work;
    prepare(work);
    use_catalog(work, Catalog{});
    work.run();
    work.strategy().answer = cannot_decide;
    const uint64_t theirs =
        create(work, goal_body(work, AYNI_TRIGGER_LOCAL_INPUT, "timer", agent_b));
    const uint64_t mine = create(work, goal_body(work, AYNI_TRIGGER_LOCAL_INPUT));
    ASSERT_EQ(post_input(work), AYNI_OK);
    work.node().step(); // takes the input
    work.node().step(); // derives both tasks
    const uint64_t their_task = task_of(work, theirs);
    const uint64_t my_task = task_of(work, mine);
    ASSERT_EQ(work.task(their_task).lifecycle, AYNI_TASK_READY);
    const auto task_before = work.task(their_task);
    const ayni_agent_info_v1 before = work.agent(agent_b);

    Catalog catalog;
    catalog.agent_b = false;
    EXPECT_EQ(restart_with(work, catalog), 3U); // fence, boot, persisted Task pause
    const ayni_agent_info_v1 agent = work.agent(agent_b);
    EXPECT_EQ(agent.lifecycle, AYNI_AGENT_FAULTED);
    EXPECT_EQ(agent.fault_reason, AYNI_REASON_AGENT_UNREGISTERED);
    EXPECT_EQ(agent.instance_id, before.instance_id);
    EXPECT_EQ(agent.revision, before.revision);
    EXPECT_EQ(agent.commit_seq, before.commit_seq);
    const ayni_task_info_v1 task = work.task(their_task);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_GT(task.commit_seq, task_before.commit_seq);
    EXPECT_EQ(task.revision, task_before.revision + 1U);
    EXPECT_EQ(task.hold_reason, AYNI_REASON_NONE);
    EXPECT_EQ(work.task(my_task).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(my_task).hold_reason, AYNI_REASON_NONE);
    EXPECT_EQ(work.agent(agent_a).fault_reason, AYNI_REASON_NONE);
    // A FAULTED instance takes no new goal.
    expect_refused(work.create(goal_body(work, AYNI_TRIGGER_NONE, "folder", agent_b)),
                   AYNI_INVALID_STATE, AYNI_REASON_OWNER_STATE, AYNI_OBJECT_AGENT_INSTANCE,
                   agent.instance_id);

    EXPECT_EQ(restart_with(work, Catalog{}), 3U); // Task resumes in startup
    EXPECT_EQ(work.agent(agent_b).lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(work.agent(agent_b).fault_reason, AYNI_REASON_NONE);
    EXPECT_EQ(work.task(their_task).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(their_task).hold_reason, AYNI_REASON_NONE);
    EXPECT_GT(work.task(their_task).commit_seq, task.commit_seq);
    EXPECT_EQ(work.task(their_task).revision, task.revision + 1U);
}

// ADR-0026 decision 8, C-LIFE-025: an instance keeps its id and revision
// across restarts, and an agent registered for the first time has its
// instance started by the transition that starts the work model.
TEST(WorkRecovery, agent_registered_for_the_first_time_starts_after_the_logged_ones) {
    WorkNode work;
    prepare(work);
    Catalog without_b;
    without_b.agent_b = false;
    use_catalog(work, without_b);
    work.run();
    const ayni_agent_info_v1 before = work.agent(agent_a);
    ASSERT_EQ(before.lifecycle, AYNI_AGENT_RUNNING);

    EXPECT_EQ(restart_with(work, Catalog{}), 3U); // the boot batch and the start
    const ayni_agent_info_v1 a = work.agent(agent_a);
    EXPECT_EQ(a.instance_id, before.instance_id);
    EXPECT_EQ(a.revision, before.revision);
    EXPECT_EQ(a.commit_seq, before.commit_seq);
    const ayni_agent_info_v1 b = work.agent(agent_b);
    EXPECT_EQ(b.lifecycle, AYNI_AGENT_RUNNING);
    EXPECT_EQ(b.revision, 1U);
    EXPECT_EQ(b.commit_seq, next_commit_seq(work) - 1U);
    EXPECT_NE(b.instance_id, 0U);
    EXPECT_NE(b.instance_id, a.instance_id);

    // With every instance logged, a restart starts none.
    EXPECT_EQ(restart_with(work, Catalog{}), 2U);
    EXPECT_EQ(work.agent(agent_b).instance_id, b.instance_id);
}

// ADR-0026 decision 5, C-LIMIT-001: a log that holds more than this boot's
// capacities restricts the kernel instead of dropping work, and none of it
// is visible.
TEST(WorkRecovery, log_beyond_the_capacities_restricts_and_shows_nothing) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
    create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
    create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
    ASSERT_EQ(work.node().stop_and_destroy(), AYNI_OK);
    work.node().config().goal_capacity = 2;
    work.node().create_and_start();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(work.node().info().restriction, AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY);
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_goal(work.kernel(), goal_id, &info), AYNI_NOT_FOUND);
    // White-box: the tables hold nothing of the log.
    const ayni::kernel::WorkModel &model = KernelTestProbe::work(work.kernel());
    EXPECT_EQ(model.goals.size(), 0U);
    EXPECT_EQ(model.agents.at(0).instance_id, 0U);
}

// ADR-0026 decision 5: ending a goal ends its live tasks in the same
// transition, so a log that ends a goal and leaves a task of it live was
// not written by this library; it is LOG_CORRUPT, and none of it is
// visible.
TEST(WorkRecovery, log_that_leaves_a_live_task_under_an_ended_goal_is_corrupt) {
    WorkNode work;
    work.run();
    work.strategy().answer = cannot_decide; // the task stays live
    const uint64_t goal_id = create(work, goal_body(work, AYNI_TRIGGER_ONCE));
    run_until_idle(work);
    ASSERT_NE(task_of(work, goal_id), 0U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, goal_id).status, AYNI_OK);
    run_until_idle(work);
    ASSERT_EQ(work.node().sim().pending_storage(), 0U);

    // Drops the task's state from the batch that cancelled the goal and it.
    StoredBatch &last = work.node().sim().log().back();
    ayni::durability::BatchView batch{};
    ASSERT_EQ(ayni::durability::read_batch(last.bytes.data(), last.bytes.size(), batch),
              ayni::durability::BatchCheck::ok);
    std::vector<uint8_t> bytes(last.bytes.size());
    ayni::durability::BatchWriter writer(bytes.data(), bytes.size(), batch.identity);
    ayni::durability::RecordReader reader(batch);
    ayni::durability::RecordHeader header{};
    const uint8_t *body = nullptr;
    uint32_t dropped = 0;
    while (reader.next(header, body) == ayni::durability::RecordReader::Next::record) {
        if (header.type == ayni::work::task_state_record_type) {
            ++dropped;
            continue;
        }
        ASSERT_TRUE(writer.add_record(header, body));
    }
    ASSERT_EQ(dropped, 1U);
    bytes.resize(writer.finish());
    last.bytes = bytes;

    work.node().reboot();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
    EXPECT_EQ(work.node().info().restriction, AYNI_RESTRICTION_LOG_CORRUPT);
    ayni_goal_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_goal(work.kernel(), goal_id, &info), AYNI_NOT_FOUND);
}

// --- Commands (decision 9) -----------------------------------------------------

// C-WIRE-003, C-WIRE-004: a command retried with the same content returns
// its first result, applies nothing and wakes no one; with other content it
// is a conflict, and neither is kept.
TEST(WorkCommands, retried_command_returns_its_first_result) {
    WorkNode work;
    work.run();
    const ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_NONE, "folder");
    const Submitted first = create_command(work, body, 5);
    ASSERT_EQ(first.status, AYNI_OK);
    const uint64_t next = next_commit_seq(work);
    const uint64_t wakes = work.node().sim().wake_count();

    const Submitted again = create_command(work, body, 5);
    EXPECT_EQ(again.status, AYNI_OK);
    EXPECT_EQ(again.result.object.id, first.result.object.id);
    EXPECT_EQ(again.result.object.revision, first.result.object.revision);
    EXPECT_EQ(again.result.commit_seq, first.result.commit_seq);
    EXPECT_EQ(next_commit_seq(work), next);
    EXPECT_EQ(work.node().sim().wake_count(), wakes);

    ayni_goal_create_v1 other = body;
    other.expiry_us = period_us;
    expect_refused(create_command(work, other, 5), AYNI_IDEMPOTENCY_CONFLICT,
                   AYNI_REASON_COMMAND_CONFLICT);
    expect_refused(create_command(work, other, 5), AYNI_IDEMPOTENCY_CONFLICT,
                   AYNI_REASON_COMMAND_CONFLICT);
    EXPECT_EQ(next_commit_seq(work), next);

    // A command of another kind with the same id conflicts too; one with a
    // new id applies.
    ayni_goal_target_v1 target{};
    target.struct_size = sizeof(target);
    target.goal_id = first.result.object.id;
    ayni_intent_v1 pause = work.intent(AYNI_INTENT_GOAL_PAUSE, &target, 1);
    pause.command_id = 5;
    expect_refused(work.submit(pause), AYNI_IDEMPOTENCY_CONFLICT, AYNI_REASON_COMMAND_CONFLICT);
    pause.command_id = 6;
    EXPECT_EQ(work.submit(pause).status, AYNI_OK);
    EXPECT_EQ(work.submit(pause).result.object.revision, 2U); // retried, not paused again
}

// A command refused by its own rules is not kept, so its retry is judged
// anew.
TEST(WorkCommands, refused_command_is_not_kept) {
    WorkNode work;
    work.run();
    ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_NONE, "folder");
    body.parent_goal_id = 12345;
    const Submitted refused = create_command(work, body, 5);
    EXPECT_EQ(refused.status, AYNI_NOT_FOUND);
    EXPECT_EQ(refused.error.reason, AYNI_REASON_PARENT_UNKNOWN);
    body.parent_goal_id = 0;
    EXPECT_EQ(create_command(work, body, 5).status, AYNI_OK);
}

// ADR-0026 decision 9: each principal keeps its last command_capacity
// commands; a new one beyond that drops the smallest id and raises the floor
// to it, so that id and every unmapped one below it are expired.
TEST(WorkCommands, full_table_drops_the_smallest_id_and_raises_the_floor) {
    WorkNode work;
    work.run();
    ASSERT_EQ(work.node().config().command_capacity, 4U);
    const ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_NONE, "folder");
    for (const uint64_t id : {20U, 10U, 30U, 40U, 50U}) {
        ASSERT_EQ(create_command(work, body, id).status, AYNI_OK) << id;
    }
    expect_refused(create_command(work, body, 10), AYNI_EXPIRED, AYNI_REASON_COMMAND_EXPIRED);
    expect_refused(create_command(work, body, 3), AYNI_EXPIRED, AYNI_REASON_COMMAND_EXPIRED);
    for (const uint64_t id : {20U, 30U, 40U, 50U}) {
        EXPECT_EQ(create_command(work, body, id).status, AYNI_OK) << id;
    }
    const uint64_t next = next_commit_seq(work);
    EXPECT_EQ(create_command(work, body, 15).status, AYNI_OK); // above the floor
    EXPECT_EQ(next_commit_seq(work), next + 1U);
    expect_refused(create_command(work, body, 15 - 1), AYNI_EXPIRED, AYNI_REASON_COMMAND_EXPIRED);
}

// ADR-0026 decision 9, C-WIRE-004: the mappings and floor are stored with
// their transitions, so a retry after a restart returns the first result,
// whichever boot gave it; one whose batch power loss took is applied anew.
TEST(WorkCommands, mappings_survive_a_restart_with_their_transitions) {
    WorkNode work;
    work.run();
    const ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_NONE, "folder");
    std::vector<Submitted> first;
    for (const uint64_t id : {1U, 2U, 3U, 4U, 5U}) {
        first.push_back(create_command(work, body, id));
        ASSERT_EQ(first.back().status, AYNI_OK);
    }
    work.node().reboot();
    work.node().recover();
    work.reopen_session();
    const Submitted retried = create_command(work, body, 5);
    EXPECT_EQ(retried.status, AYNI_OK);
    EXPECT_EQ(retried.result.object.id, first[4].result.object.id);
    EXPECT_EQ(retried.result.commit_seq, first[4].result.commit_seq);
    expect_refused(create_command(work, body, 1), AYNI_EXPIRED, AYNI_REASON_COMMAND_EXPIRED);

    const Submitted unstored = create_command(work, body, 6);
    ASSERT_EQ(unstored.status, AYNI_OK);
    work.node().power_cut_and_start(CutAppend::lost);
    work.node().recover();
    work.reopen_session();
    const Submitted anew = create_command(work, body, 6);
    EXPECT_EQ(anew.status, AYNI_OK);
    EXPECT_NE(anew.result.object.id, unstored.result.object.id);
}

// ADR-0026 decision 9: each mapping's record carries the floor, so a later
// boot with room for more commands still expires the unmapped ids at or below
// it. The mappings it replays all fit, so it keeps the one the earlier boot
// dropped and returns its first result.
TEST(WorkCommands, floor_survives_a_restart_with_more_room) {
    WorkNode work;
    work.run();
    const ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_NONE, "folder");
    for (const uint64_t id : {20U, 10U, 30U, 40U, 50U}) {
        ASSERT_EQ(create_command(work, body, id).status, AYNI_OK) << id;
    }
    expect_refused(create_command(work, body, 10), AYNI_EXPIRED, AYNI_REASON_COMMAND_EXPIRED);
    ASSERT_EQ(work.node().stop_and_destroy(), AYNI_OK);
    work.node().config().command_capacity = 8;
    work.node().create_and_start();
    work.node().recover();
    work.reopen_session();
    expect_refused(create_command(work, body, 5), AYNI_EXPIRED, AYNI_REASON_COMMAND_EXPIRED);
    const uint64_t next = next_commit_seq(work);
    EXPECT_EQ(create_command(work, body, 10).status, AYNI_OK);
    EXPECT_EQ(next_commit_seq(work), next); // retried
}

// --- Room in the journal ------------------------------------------------------

// ADR-0026: a transition needs room for the largest batch before it starts.
// Without room an intent is BUSY, and due work and decisions wait until an
// append's result frees some; the step then reports nothing ready.
TEST(WorkJournal, transition_waits_for_room_for_its_batch) {
    WorkNode work;
    work.node().config().storage_buffer_bytes =
        smallest_storage_buffer(work.node().config()); // one batch
    work.run();
    const uint64_t goal_id = create(work, goal_body(work, AYNI_TRIGGER_ONCE));
    const uint64_t next = next_commit_seq(work);
    const ayni_goal_create_v1 body = goal_body(work, AYNI_TRIGGER_NONE, "folder");
    expect_refused(work.create(body), AYNI_BUSY, AYNI_REASON_STORAGE_BACKLOG);
    expect_refused(create_command(work, body, 8), AYNI_BUSY, AYNI_REASON_STORAGE_BACKLOG);
    EXPECT_EQ(next_commit_seq(work), next);

    ayni_step_report_v1 report = storage_step(work); // issues the append, derives nothing
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    EXPECT_EQ(task_of(work, goal_id), 0U);
    expect_refused(work.create(body), AYNI_BUSY, AYNI_REASON_STORAGE_BACKLOG);

    const uint64_t wakes = work.node().sim().wake_count();
    ASSERT_EQ(work.node().sim().complete_storage(), 1U);
    EXPECT_EQ(work.node().sim().wake_count(), wakes + 1U);
    report = storage_step(work); // the append's result, then the derivation
    EXPECT_EQ(report.items_done, 2U);
    EXPECT_NE(task_of(work, goal_id), 0U);
    // A command refused BUSY is not kept, so its retry applies.
    run_until_idle(work);
    EXPECT_EQ(create_command(work, body, 8).status, AYNI_OK);
}

// ADR-0026: an event in RUNNING waits for room too, queued, and is taken
// once the append's result frees some.
TEST(WorkJournal, event_waits_for_room_queued) {
    WorkNode work;
    work.node().config().storage_buffer_bytes = smallest_storage_buffer(work.node().config());
    work.run();
    const uint64_t goal_id = create(work, goal_body(work, AYNI_TRIGGER_LOCAL_INPUT));
    ASSERT_EQ(post_input(work), AYNI_OK);
    EXPECT_EQ(storage_step(work).items_done, 0U);
    EXPECT_EQ(work.node().info().events_discarded, 0U);
    EXPECT_EQ(task_of(work, goal_id), 0U);
    work.node().sim().complete_storage();
    EXPECT_EQ(storage_step(work).items_done, 2U); // the result, then the event
    EXPECT_EQ(work.goal(goal_id).next_due_us, work.node().sim().now_us());
    EXPECT_EQ(work.node().sim().complete_storage(), 0U); // the write budget has not opened
    storage_step(work); // issues the event's batch at its storage deadline
    ASSERT_EQ(work.node().sim().complete_storage(), 1U);
    storage_step(work); // the result, then the derivation
    EXPECT_NE(task_of(work, goal_id), 0U);
}

// C-COMMIT-004, ADR-0026 decision 3: an accepted intent is applied at once
// and durable once the Host has stored its batch: durable_seq reaches its
// commit_seq when a step applies the append's result, in the kernel info and
// in the step report alike.
TEST(WorkJournal, result_is_durable_once_the_host_stores_its_batch) {
    WorkNode work;
    work.run();
    const uint64_t durable = work.node().info().durable_seq;
    const Submitted created = work.create(goal_body(work, AYNI_TRIGGER_NONE, "folder"));
    ASSERT_EQ(created.status, AYNI_OK);
    EXPECT_GT(created.result.commit_seq, durable);
    ayni_step_report_v1 report = storage_step(work); // issues the append
    EXPECT_EQ(report.durable_seq, durable);
    ASSERT_EQ(work.node().sim().complete_storage(), 1U);
    EXPECT_EQ(work.node().info().durable_seq, durable); // posted, not yet applied
    report = storage_step(work);
    EXPECT_EQ(report.durable_seq, created.result.commit_seq);
    EXPECT_EQ(work.node().info().durable_seq, created.result.commit_seq);
}

// --- Append failures ---------------------------------------------------------

// C-COMMIT-008/010: only exhausted retries enter FENCED. Failure to
// isolate then leaves read-only diagnostics, without storing later work.
TEST(WorkJournal, append_that_fails_restricts_the_kernel) {
    const struct {
        StorageFault fault;
        ayni_restriction_t restriction;
        bool stored;
    } cases[] = {
        {StorageFault::fail, AYNI_RESTRICTION_STORAGE_WRITE_FAILED, false},
        {StorageFault::unknown_lost, AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN, false},
        {StorageFault::unknown_persisted, AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN, true},
        {StorageFault::reject, AYNI_RESTRICTION_STORAGE_REJECTED, false},
    };
    for (const auto &c : cases) {
        SCOPED_TRACE(static_cast<int>(c.fault));
        WorkNode work;
        work.run();
        const uint64_t durable = work.node().info().durable_seq;
        const uint64_t goal_id = create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
        for (int attempt = 0; attempt < 3; ++attempt) {
            work.node().sim().inject_fault(StoragePort::append, attempt != 0 && c.stored
                                                                    ? StorageFault::unknown_lost
                                                                    : c.fault);
        }
        for (int round = 0; round < 12 && work.node().lifecycle() == AYNI_LIFECYCLE_RUNNING;
             ++round) {
            storage_step(work);
            work.node().sim().complete_storage();
            work.node().step(1);
        }
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_FENCED);
        EXPECT_EQ(work.node().info().restriction, c.restriction);
        EXPECT_EQ(work.node().info().durable_seq, durable);
        work.node().sim().inject_fault(StoragePort::fence, StorageFault::unavailable);
        run_until_idle(work);
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RESTRICTED);
        const std::size_t stored = work.node().sim().log().size();
        work.node().step();
        EXPECT_EQ(work.node().sim().pending_storage(), 0U);
        EXPECT_EQ(work.node().sim().log().size(), stored);

        work.node().reboot();
        work.node().recover();
        ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
        ayni_goal_info_v1 info{};
        info.struct_size = sizeof(info);
        EXPECT_EQ(ayni_get_goal(work.kernel(), goal_id, &info),
                  c.stored ? AYNI_OK : AYNI_NOT_FOUND);
    }
}

// --- Shutdown ----------------------------------------------------------------

// ADR-0026: DRAINING appends the batches RUNNING committed, one at a time,
// and stops once the last is stored; the next boot sees all of them.
TEST(WorkJournal, draining_stores_every_batch_before_it_stops) {
    WorkNode work;
    work.run();
    std::vector<uint64_t> goals;
    for (int goal = 0; goal < 3; ++goal) {
        goals.push_back(create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder")));
    }
    const uint64_t last_seq = next_commit_seq(work) - 1U;
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    EXPECT_EQ(storage_step(work).more_ready, 0U); // waits for the append
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_DRAINING);
    work.node().drain(); // later requests respect the shared write interval
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_EQ(work.node().info().durable_seq, last_seq);
    EXPECT_EQ(work.node().sim().log().size(), last_seq);

    work.node().reboot();
    work.node().recover();
    for (const uint64_t goal_id : goals) {
        EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);
        EXPECT_EQ(work.goal(goal_id).hold_reason, AYNI_REASON_NONE);
    }
}

// An append that fails while draining ends the writer: the kernel stops
// without storing the rest.
TEST(WorkJournal, draining_stops_when_an_append_fails) {
    WorkNode work;
    work.run();
    create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
    create(work, goal_body(work, AYNI_TRIGGER_NONE, "folder"));
    ASSERT_EQ(ayni_begin_shutdown(work.kernel()), AYNI_OK);
    for (int attempt = 0; attempt < 3; ++attempt) {
        work.node().sim().inject_fault(StoragePort::append, StorageFault::fail);
    }
    work.node().drain();
    EXPECT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_EQ(work.node().sim().pending_storage(), 0U);
    EXPECT_EQ(work.node().info().durable_seq, 3U);
}

// C-LIFE-024 / ADR-0038: an incompatible ancestor's type is a volatile hold
// for its descendant's Task, even when the child's own type is registered.
// Compatibility recovery must preserve the stored Task revision and reason.
TEST(WorkRecovery, incompatible_parent_type_holds_a_compatible_child_without_rewriting_its_state) {
    WorkNode work;
    work.run();
    const auto parent = work.create(WorkNode::create_body(work.instance(agent_a), 0, "reminder"));
    ASSERT_EQ(parent.status, AYNI_OK);
    const auto child = work.create(WorkNode::create_body(
        work.instance(agent_a), parent.result.object.id, "timer", 1, {}, AYNI_TRIGGER_ONCE));
    ASSERT_EQ(child.status, AYNI_OK);
    work.node().step(1);
    work.node().recover();
    const auto task_id = work.goal(child.result.object.id).last_task_id;
    ASSERT_NE(task_id, 0U);
    const auto before = work.task(task_id);
    Catalog missing;
    missing.reminder_version = 0;
    restart_with(work, missing);
    const auto held = work.task(task_id);
    EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(held.hold_status, AYNI_NOT_FOUND);
    EXPECT_EQ(held.hold_reason, AYNI_REASON_GOAL_TYPE_UNKNOWN);
    EXPECT_EQ(held.revision, before.revision);
    EXPECT_EQ(held.commit_seq, before.commit_seq);
    EXPECT_EQ(work.node().step(1).more_ready, 0U);
    EXPECT_TRUE(work.strategy().seen.empty());
    restart_with(work, Catalog{});
    EXPECT_EQ(work.task(task_id).lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(work.task(task_id).revision, before.revision);
    EXPECT_EQ(work.task(task_id).commit_seq, before.commit_seq);
    work.strategy().answer = {};
    work.node().step(1);
    ASSERT_EQ(work.strategy().seen.size(), 1U);
    EXPECT_EQ(work.strategy().seen.front().snapshot.task_id, task_id);
}

// C-POLICY-010 / ADR-0038: equal versions do not make different strategy
// identities compatible with a private checkpoint. Returning the original
// writer continues exactly the saved Task, steps and bytes.
TEST(WorkRecovery, another_strategy_at_the_same_version_cannot_read_the_saved_checkpoint) {
    WorkNode work;
    const auto kept = live_tasks(work);
    const auto before = work.task(kept.reminder_task);
    Catalog changed;
    changed.strategy_name = "other-strategy";
    restart_with(work, changed);
    const auto held = work.task(kept.reminder_task);
    EXPECT_EQ(held.lifecycle, AYNI_TASK_PAUSED);
    EXPECT_EQ(held.hold_status, AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(held.hold_reason, AYNI_REASON_STRATEGY_CHANGED);
    EXPECT_EQ(held.revision, before.revision);
    EXPECT_EQ(held.commit_seq, before.commit_seq);
    work.strategy().seen.clear();
    EXPECT_EQ(work.node().step(1).items_done, 1U); // Compatible timer may fail its own policy.
    for (const auto &seen : work.strategy().seen) {
        EXPECT_NE(seen.snapshot.task_id, kept.reminder_task);
    }
    restart_with(work, Catalog{});
    EXPECT_EQ(work.task(kept.reminder_task).lifecycle, AYNI_TASK_READY);
    work.strategy().seen.clear();
    work.strategy().answer = {};
    work.node().step(1);
    ASSERT_EQ(work.strategy().seen.size(), 1U);
    EXPECT_EQ(work.strategy().seen.front().snapshot.task_id, kept.reminder_task);
    EXPECT_EQ(work.strategy().seen.front().checkpoint_strategy_id, strategy_id);
    EXPECT_EQ(work.strategy().seen.front().checkpoint, (std::vector<uint8_t>{7, 1}));
    EXPECT_EQ(work.strategy().seen.front().steps, (std::vector<std::string>{"turn0", "turn1"}));
}

} // namespace
