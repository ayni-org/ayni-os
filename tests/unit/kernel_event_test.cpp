// Events through the C ABI (ADR-0021): which lifecycle states take a local
// input and what each does with it, the order of ayni_post_event's checks, a
// full queue, the step's budgets, posts from Host threads, and goals whose
// LOCAL_INPUT trigger records an occurrence for each input from their
// source. Runs on the simulation Host, whose clock moves only when a test
// moves it.

#include "kernel_node.h"
#include "work_node.h"

#include <ayni/ayni.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {

using namespace ayni::test;

constexpr uint64_t no_time = std::numeric_limits<uint64_t>::max();
constexpr uint64_t second = AYNI_TRIGGER_PERIOD_MIN_US;

ayni_event_v1 local_input(ayni_string_v1 source) {
    ayni_event_v1 event{};
    event.struct_size = sizeof(event);
    event.kind = AYNI_EVENT_LOCAL_INPUT;
    event.source = source;
    return event;
}

ayni_status_t post(ayni_kernel_t *kernel, const char *source = "button") {
    const ayni_event_v1 event = local_input(text(source));
    return ayni_post_event(kernel, &event);
}

// --- Lifecycle -----------------------------------------------------------------

// ADR-0021: ayni_start opens the gate and STOPPED closes it; only a queued
// event wakes the owner.
TEST(KernelEvents, states_between_start_and_stopped_take_posts) {
    const struct {
        ayni_lifecycle_t state;
        ayni_status_t status;
    } cases[] = {
        {AYNI_LIFECYCLE_CONFIGURING, AYNI_INVALID_STATE},
        {AYNI_LIFECYCLE_RECOVERING, AYNI_OK},
        {AYNI_LIFECYCLE_RUNNING, AYNI_OK},
        {AYNI_LIFECYCLE_RESTRICTED, AYNI_OK},
        {AYNI_LIFECYCLE_DRAINING, AYNI_OK},
        {AYNI_LIFECYCLE_STOPPED, AYNI_INVALID_STATE},
    };
    for (const auto &[state, status] : cases) {
        SCOPED_TRACE(state);
        Node node;
        node.enter(state);
        const uint64_t wakes = node.sim().wake_count();
        EXPECT_EQ(post(node.kernel()), status);
        EXPECT_EQ(node.sim().wake_count(), wakes + (status == AYNI_OK ? 1U : 0U));
        EXPECT_EQ(node.sim().calls_under_lock(), 0U);
    }
}

// ayni_post_event checks its arguments, then the kind, then the source, and
// the state only after them; a refusal queues nothing and wakes nobody.
TEST(KernelEvents, post_checks_arguments_kind_and_source_before_the_state) {
    Node node;
    node.run();
    const uint64_t wakes = node.sim().wake_count();
    const ayni_event_v1 valid = local_input(text("button"));
    EXPECT_EQ(ayni_post_event(nullptr, &valid), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ayni_post_event(node.kernel(), nullptr), AYNI_INVALID_ARGUMENT);

    ayni_event_v1 event = valid;
    for (const uint32_t size : {0U, static_cast<uint32_t>(sizeof(event) - 1U)}) {
        event.struct_size = size;
        EXPECT_EQ(ayni_post_event(node.kernel(), &event), AYNI_INVALID_ARGUMENT) << size;
    }
    event.struct_size = sizeof(event) + 8U;
    event.kind = 0;
    EXPECT_EQ(ayni_post_event(node.kernel(), &event), AYNI_UNSUPPORTED_VERSION);
    event.struct_size = sizeof(event);
    EXPECT_EQ(ayni_post_event(node.kernel(), &event), AYNI_INVALID_ARGUMENT);

    // Kinds the draft names but this library does not implement, and kinds
    // nobody defines, are refused before their source is read.
    event.source = {nullptr, 6};
    for (const ayni_event_kind_t kind : {1U, 2U, 4U, 99U, 0xFFFFFFFFU}) {
        event.kind = kind;
        EXPECT_EQ(ayni_post_event(node.kernel(), &event), AYNI_UNSUPPORTED) << kind;
    }

    const ayni_string_v1 bad_sources[] = {
        {nullptr, 0},
        {nullptr, 6},
        text("Button"),
        text("big button"),
        text("abcdefghijklmnopqrstuvwxyz0123456"),
    };
    for (const ayni_string_v1 source : bad_sources) {
        event = local_input(source);
        EXPECT_EQ(ayni_post_event(node.kernel(), &event), AYNI_INVALID_ARGUMENT) << source.size;
    }
    EXPECT_EQ(node.sim().wake_count(), wakes);
    const ayni_step_report_v1 report = node.step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(node.info().events_unmatched, 0U);

    Node configuring;
    configuring.create();
    event = local_input(text("Button"));
    EXPECT_EQ(ayni_post_event(configuring.kernel(), &event), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(post(configuring.kernel()), AYNI_INVALID_STATE);
}

// C-SUB-004: a local input is never dropped to make room; a full queue
// refuses it, and the post after a step that took one is queued again.
TEST(KernelEvents, full_queue_refuses_until_a_step_takes_one) {
    Node node;
    node.run();
    const uint64_t wakes = node.sim().wake_count();
    EXPECT_EQ(post(node.kernel()), AYNI_OK);
    EXPECT_EQ(post(node.kernel(), "knob"), AYNI_OK);
    EXPECT_EQ(post(node.kernel()), AYNI_BUSY);
    EXPECT_EQ(node.sim().wake_count(), wakes + 2U);

    ayni_step_report_v1 report = node.step(1);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_EQ(post(node.kernel()), AYNI_OK);
    EXPECT_EQ(post(node.kernel()), AYNI_BUSY);
    report = node.step();
    EXPECT_EQ(report.items_done, 2U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(node.info().events_unmatched, 3U);
    EXPECT_EQ(node.info().events_discarded, 0U);
}

// Each event is one item, and the clock is read after each.
TEST(KernelEvents, events_beyond_the_step_budget_wait_for_the_next_step) {
    Node node;
    node.config().ingress_capacity = 4;
    node.run();
    for (int event = 0; event < 4; ++event) {
        ASSERT_EQ(post(node.kernel()), AYNI_OK);
    }
    ayni_step_report_v1 report = node.step(1);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);

    node.sim().advance_per_read_us(10);
    report = node.step(unbounded_items, 5);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    node.sim().advance_per_read_us(0);

    report = node.step();
    EXPECT_EQ(report.items_done, 2U);
    EXPECT_EQ(report.more_ready, 0U);
    EXPECT_EQ(node.info().events_unmatched, 4U);
}

// RECOVERING keeps its events, without asking for a step, for RUNNING to
// apply once recovery ends.
TEST(KernelEvents, recovering_keeps_events_for_running) {
    Node node;
    node.enter(AYNI_LIFECYCLE_RECOVERING);
    ASSERT_EQ(post(node.kernel()), AYNI_OK);
    const ayni_step_report_v1 report = node.step();
    EXPECT_EQ(report.items_done, 0U);
    EXPECT_EQ(report.more_ready, 0U);
    ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RECOVERING);
    EXPECT_EQ(node.info().events_unmatched, 0U);
    EXPECT_EQ(node.info().events_discarded, 0U);

    node.recover();
    ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_RUNNING);
    EXPECT_EQ(node.info().events_unmatched, 1U);
    EXPECT_EQ(node.info().events_discarded, 0U);
}

// RESTRICTED and DRAINING take no new work, so their events are discarded
// and counted, each still one item.
TEST(KernelEvents, restricted_and_draining_discard_events) {
    for (const ayni_lifecycle_t state : {AYNI_LIFECYCLE_RESTRICTED, AYNI_LIFECYCLE_DRAINING}) {
        SCOPED_TRACE(state);
        Node node;
        node.enter(state);
        ASSERT_EQ(post(node.kernel()), AYNI_OK);
        ASSERT_EQ(post(node.kernel()), AYNI_OK);
        const ayni_step_report_v1 report = node.step();
        EXPECT_EQ(report.items_done, 2U);
        EXPECT_EQ(report.more_ready, 0U);
        EXPECT_EQ(node.lifecycle(), state);
        EXPECT_EQ(node.info().events_discarded, 2U);
        EXPECT_EQ(node.info().events_unmatched, 0U);
    }
}

// Entering STOPPED closes the gate for good: the events still queued are
// counted as discarded, and later posts are refused.
TEST(KernelEvents, stopping_counts_the_events_left_queued) {
    Node node;
    node.enter(AYNI_LIFECYCLE_DRAINING);
    ASSERT_EQ(post(node.kernel()), AYNI_OK);
    ASSERT_EQ(post(node.kernel()), AYNI_OK);
    ASSERT_EQ(node.sim().complete_storage(), 1U);
    // The completion takes the only item, so the step reaches STOPPED with
    // both events queued.
    const ayni_step_report_v1 report = node.step(1);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 0U);
    ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_STOPPED);
    EXPECT_EQ(node.info().events_discarded, 2U);

    const uint64_t wakes = node.sim().wake_count();
    EXPECT_EQ(post(node.kernel()), AYNI_INVALID_STATE);
    EXPECT_EQ(node.sim().wake_count(), wakes);
    EXPECT_EQ(node.info().events_discarded, 2U);
}

// The kernel that posting_clock posts to; only the test using it sets it.
ayni_kernel_t *posting_kernel = nullptr;

// A Host clock that posts a local input each time the kernel reads it, as a
// Host thread posting while the step runs would; a full queue refuses some.
// ayni.h forbids a Host to post from a function the kernel calls; this
// stands in for another thread only because the kernel never reads the
// clock while it holds the ingress lock.
uint64_t posting_clock(void *ctx) {
    if (posting_kernel != nullptr) {
        const ayni_status_t status = post(posting_kernel);
        EXPECT_TRUE(status == AYNI_OK || status == AYNI_BUSY) << status;
    }
    return static_cast<ayni::hosts::sim::Host *>(ctx)->now_us();
}

// A step takes only the events queued when it comes to them, after the due
// work, so a Host that keeps posting cannot hold it there.
TEST(KernelEvents, events_posted_during_a_step_wait_for_the_next) {
    WorkNode work;
    work.node().replace_clock_port(&posting_clock);
    work.run();
    ayni_goal_create_v1 once = WorkNode::create_body(work.instance(agent_a), 0, "timer", 1,
                                                     {nullptr, 0}, AYNI_TRIGGER_ONCE);
    ASSERT_EQ(work.create(once).status, AYNI_OK);
    ASSERT_EQ(post(work.kernel()), AYNI_OK);
    posting_kernel = work.kernel();
    const ayni_step_report_v1 report = work.node().step();
    posting_kernel = nullptr;
    // The task, then the events posted before the step and as it started;
    // the next step decides the task and takes the events posted during
    // this one.
    EXPECT_EQ(report.items_done, 3U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_EQ(work.node().step().items_done, 3U);
    EXPECT_EQ(work.node().info().events_unmatched, 4U);
}

// --- Host threads ----------------------------------------------------------------

// Bounded so that a stuck owner fails the test instead of hanging it.
std::chrono::steady_clock::time_point ten_seconds_from_now() {
    return std::chrono::steady_clock::now() + std::chrono::seconds(10);
}

// ADR-0015, ADR-0021: Host threads post while the owner steps. Every post
// that returns AYNI_OK is taken once and wakes the owner once, and no Host
// function runs under the ingress lock. Under TSan this also catches queue
// or gate state touched outside the lock.
TEST(KernelEvents, posts_from_host_threads_are_each_taken_once) {
    constexpr uint32_t producers = 4;
    constexpr uint32_t posts_each = 500;
    Node node;
    node.config().ingress_capacity = 8;
    node.run();
    const uint64_t wakes = node.sim().wake_count();
    const auto deadline = ten_seconds_from_now();
    std::atomic<uint32_t> finished{0};
    std::vector<std::thread> threads;
    for (uint32_t thread = 0; thread < producers; ++thread) {
        threads.emplace_back([&, thread] {
            const char *source = thread % 2U == 0U ? "button" : "knob";
            uint32_t queued = 0;
            while (queued < posts_each && std::chrono::steady_clock::now() < deadline) {
                const ayni_status_t status = post(node.kernel(), source);
                if (status == AYNI_OK) {
                    ++queued;
                } else if (status == AYNI_BUSY) {
                    std::this_thread::yield();
                } else {
                    ADD_FAILURE() << "post returned " << status;
                    break;
                }
            }
            EXPECT_EQ(queued, posts_each);
            finished.fetch_add(1);
        });
    }
    constexpr uint64_t total = uint64_t{producers} * posts_each;
    while ((finished.load() < producers || node.info().events_unmatched < total) &&
           std::chrono::steady_clock::now() < deadline) {
        node.step(3);
        std::this_thread::yield();
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(node.info().events_unmatched, total);
    EXPECT_EQ(node.step().items_done, 0U);
    EXPECT_EQ(node.sim().wake_count(), wakes + total);
    EXPECT_EQ(node.sim().calls_under_lock(), 0U);
}

// Host threads post while the owner shuts down. Each post that returned
// AYNI_OK is counted exactly once, as taken in RUNNING, discarded in
// DRAINING or left queued at STOPPED, and every thread sees the gate close.
TEST(KernelEvents, posts_racing_shutdown_are_each_counted_once) {
    constexpr uint32_t producers = 4;
    const auto deadline = ten_seconds_from_now(); // for all rounds together
    for (int round = 0; round < 20; ++round) {
        SCOPED_TRACE(round);
        Node node;
        node.config().ingress_capacity = 8;
        node.run();
        std::atomic<uint64_t> accepted{0};
        std::vector<std::thread> threads;
        for (uint32_t thread = 0; thread < producers; ++thread) {
            threads.emplace_back([&] {
                bool closed = false;
                while (!closed && std::chrono::steady_clock::now() < deadline) {
                    const ayni_status_t status = post(node.kernel());
                    if (status == AYNI_OK) {
                        accepted.fetch_add(1);
                    } else if (status == AYNI_BUSY) {
                        std::this_thread::yield();
                    } else {
                        EXPECT_EQ(status, AYNI_INVALID_STATE);
                        closed = true;
                    }
                }
                EXPECT_TRUE(closed);
            });
        }
        while (accepted.load() < 50U && std::chrono::steady_clock::now() < deadline) {
            node.step(2);
        }
        ASSERT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
        while (node.lifecycle() == AYNI_LIFECYCLE_DRAINING &&
               std::chrono::steady_clock::now() < deadline) {
            node.step(2);
        }
        for (std::thread &thread : threads) {
            thread.join();
        }
        ASSERT_EQ(node.lifecycle(), AYNI_LIFECYCLE_STOPPED);
        const ayni_kernel_info_v1 info = node.info();
        // At most a queue's worth of the first 50 was still queued.
        EXPECT_GE(info.events_unmatched, 50U - 8U);
        EXPECT_EQ(info.events_unmatched + info.events_discarded, accepted.load());
        EXPECT_EQ(node.sim().calls_under_lock(), 0U);
    }
}

// --- Local input triggers ------------------------------------------------------

ayni_goal_create_v1 input_body(WorkNode &work, ayni_string_v1 source, const char *type = "timer",
                               uint64_t agent_id = agent_a) {
    ayni_goal_create_v1 body = WorkNode::create_body(work.instance(agent_id), 0, type, 1,
                                                     {nullptr, 0}, AYNI_TRIGGER_LOCAL_INPUT);
    body.trigger_source = source;
    return body;
}

uint64_t create(WorkNode &work, const ayni_goal_create_v1 &body) {
    const Submitted created = work.create(body);
    EXPECT_EQ(created.status, AYNI_OK);
    return created.result.object.id;
}

uint64_t input_goal(WorkNode &work, const char *source, const char *type = "timer",
                    uint64_t agent_id = agent_a) {
    return create(work, input_body(work, text(source), type, agent_id));
}

uint64_t now(WorkNode &work) {
    return work.node().sim().now_us();
}

void advance(WorkNode &work, uint64_t delta_us) {
    work.node().sim().advance_us(delta_us);
}

ayni_status_t post(WorkNode &work, const char *source = "button") {
    return post(work.kernel(), source);
}

// ADR-0021: a goal has nothing due until an input from its source arrives;
// each input is then an occurrence due when the step takes it, which becomes
// a task in that step, and the revision stays.
TEST(WorkInput, goal_takes_inputs_from_its_source) {
    WorkNode work;
    work.run();
    advance(work, 1000);
    const uint64_t wakes = work.node().sim().wake_count();
    const uint64_t goal_id = input_goal(work, "button");
    EXPECT_EQ(work.node().sim().wake_count(), wakes); // nothing falls due
    const ayni_goal_info_v1 created = work.goal(goal_id);
    EXPECT_EQ(created.trigger, AYNI_TRIGGER_LOCAL_INPUT);
    ASSERT_EQ(created.trigger_source_size, 6U);
    EXPECT_EQ(std::memcmp(created.trigger_source, "button", 6), 0);
    EXPECT_EQ(created.trigger_delay_us, 0U);
    EXPECT_EQ(created.trigger_period_us, 0U);
    EXPECT_EQ(created.next_due_us, no_time);
    EXPECT_EQ(created.wait_reason, AYNI_REASON_NONE);
    EXPECT_EQ(created.reserved, 0U);
    EXPECT_EQ(work.node().step().next_deadline_us, no_time);

    advance(work, 500);
    const uint64_t pressed_at = now(work);
    ASSERT_EQ(post(work), AYNI_OK);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U); // the occurrence is due
    EXPECT_EQ(report.next_deadline_us, pressed_at);
    const ayni_goal_info_v1 recorded = work.goal(goal_id);
    EXPECT_EQ(recorded.next_due_us, pressed_at);
    EXPECT_EQ(recorded.occurrences, 0U);
    EXPECT_EQ(recorded.revision, created.revision);
    EXPECT_GT(recorded.commit_seq, created.commit_seq);

    // The next step derives the task, still due when the input was taken.
    advance(work, 300);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U); // the task awaits its decision
    EXPECT_EQ(report.next_deadline_us, no_time);
    const ayni_goal_info_v1 derived = work.goal(goal_id);
    EXPECT_EQ(derived.occurrences, 1U);
    EXPECT_EQ(derived.next_due_us, no_time);
    EXPECT_EQ(derived.revision, created.revision);
    EXPECT_GT(derived.commit_seq, recorded.commit_seq);
    const ayni_task_info_v1 task = work.task(derived.last_task_id);
    EXPECT_EQ(task.lifecycle, AYNI_TASK_READY);
    EXPECT_EQ(task.occurrence, 1U);
    EXPECT_EQ(task.occurrence_count, 1U);
    EXPECT_EQ(task.due_us, pressed_at);

    // An input from another source reaches no goal; the step also decides
    // the task, which leaves the goal for its next input.
    ASSERT_EQ(post(work, "knob"), AYNI_OK);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 2U);
    EXPECT_EQ(work.node().info().events_unmatched, 1U);
    EXPECT_EQ(work.goal(goal_id).commit_seq, derived.commit_seq);
    EXPECT_EQ(work.goal(goal_id).lifecycle, AYNI_GOAL_ACTIVE);

    advance(work, 500);
    ASSERT_EQ(post(work), AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.node().step().items_done, 1U);
    const ayni_task_info_v1 second_task = work.task(work.goal(goal_id).last_task_id);
    EXPECT_EQ(second_task.occurrence, 2U);
    EXPECT_EQ(second_task.due_us, pressed_at + 800U);
}

// ADR-0023: a LOCAL_INPUT goal has an occurrence to come while another
// input would have a number, so cancelling its one task leaves it ACTIVE
// with its revision, and the next input becomes a task.
TEST(WorkInput, task_cancel_leaves_the_goal_taking_inputs) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = input_goal(work, "button");
    ASSERT_EQ(post(work), AYNI_OK);
    ASSERT_EQ(work.node().step().items_done, 1U);
    ASSERT_EQ(work.node().step(1).items_done, 1U);
    const uint64_t task_id = work.goal(goal_id).last_task_id;
    const ayni_goal_info_v1 before = work.goal(goal_id);
    ASSERT_EQ(before.next_due_us, no_time);

    ASSERT_EQ(work.cancel_task(task_id).status, AYNI_OK);
    const ayni_goal_info_v1 after = work.goal(goal_id);
    EXPECT_EQ(after.lifecycle, AYNI_GOAL_ACTIVE);
    EXPECT_EQ(after.revision, before.revision);
    EXPECT_EQ(after.commit_seq, before.commit_seq);
    ASSERT_EQ(post(work), AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.task(work.goal(goal_id).last_task_id).occurrence, 2U);
}

// Due work comes before events in a step, so a Host that keeps the queue
// full cannot hold back a goal's expiry or its tasks.
TEST(WorkInput, steady_events_do_not_hold_back_due_work) {
    WorkNode work;
    work.run();
    const uint64_t once = create(work, WorkNode::create_body(work.instance(agent_a), 0, "timer", 1,
                                                             {nullptr, 0}, AYNI_TRIGGER_ONCE));
    ayni_goal_create_v1 expiring_body = WorkNode::create_body(work.instance(agent_b), 0, "timer");
    expiring_body.expiry_us = 10;
    const uint64_t expiring = create(work, expiring_body);
    for (int round = 0; round < 2; ++round) {
        while (post(work, "knob") == AYNI_OK) {
        }
        advance(work, 10);
        EXPECT_EQ(work.node().step(1).items_done, 1U);
    }
    EXPECT_EQ(work.goal(expiring).lifecycle, AYNI_GOAL_EXPIRED);
    EXPECT_NE(work.goal(once).last_task_id, 0U);
    EXPECT_EQ(work.node().info().events_unmatched, 0U);
    // Decisions come before events too (ADR-0022).
    EXPECT_EQ(work.node().step(1).items_done, 1U);
    EXPECT_EQ(work.task(work.goal(once).last_task_id).lifecycle, AYNI_TASK_COMPLETED);
    EXPECT_EQ(work.node().info().events_unmatched, 0U);
    const ayni_step_report_v1 report = work.node().step(1);
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.more_ready, 1U);
    EXPECT_EQ(work.node().info().events_unmatched, 1U);
}

// One input records an occurrence on every live goal that takes its source,
// paused ones included, in one transition. Paused here, so that nothing
// derives in between.
TEST(WorkInput, one_input_reaches_every_matching_goal_in_one_transition) {
    WorkNode work;
    work.run();
    const uint64_t first = input_goal(work, "button", "timer", agent_a);
    const uint64_t second_goal = input_goal(work, "button", "reminder", agent_b);
    const uint64_t other = input_goal(work, "knob");
    for (const uint64_t goal_id : {first, second_goal}) {
        ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    }
    const uint64_t paused_seq = work.goal(second_goal).commit_seq;
    const uint64_t other_seq = work.goal(other).commit_seq;

    advance(work, 700);
    const uint64_t pressed_at = now(work);
    ASSERT_EQ(post(work), AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    const ayni_goal_info_v1 a = work.goal(first);
    const ayni_goal_info_v1 b = work.goal(second_goal);
    EXPECT_EQ(a.commit_seq, paused_seq + 1U);
    EXPECT_EQ(b.commit_seq, a.commit_seq);
    for (const ayni_goal_info_v1 &goal : {a, b}) {
        EXPECT_EQ(goal.lifecycle, AYNI_GOAL_PAUSED);
        EXPECT_EQ(goal.next_due_us, pressed_at);
        EXPECT_EQ(goal.occurrences, 0U);
        EXPECT_EQ(goal.revision, 2U);
    }
    EXPECT_EQ(work.goal(other).commit_seq, other_seq);
    EXPECT_EQ(work.goal(other).next_due_us, no_time);
    EXPECT_EQ(work.node().info().events_unmatched, 0U);

    for (const uint64_t goal_id : {first, second_goal}) {
        ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    }
    EXPECT_EQ(work.node().step().items_done, 2U);
    for (const uint64_t goal_id : {first, second_goal}) {
        const ayni_task_info_v1 task = work.task(work.goal(goal_id).last_task_id);
        EXPECT_EQ(task.occurrence, 1U) << goal_id;
        EXPECT_EQ(task.due_us, pressed_at) << goal_id;
    }
}

// C-PROD-018, C-LIMIT-007: inputs that wait while the goal is paused become
// one task on resume, naming the latest; the goal is due from the earliest.
TEST(WorkInput, paused_goal_backlog_becomes_one_task) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = input_goal(work, "button");
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    advance(work, 100);
    const uint64_t first_at = now(work);
    for (int press = 0; press < 3; ++press) {
        ASSERT_EQ(post(work), AYNI_OK);
        EXPECT_EQ(work.node().step().items_done, 1U);
        advance(work, 100);
    }
    const uint64_t last_at = first_at + 200U;
    EXPECT_EQ(work.goal(goal_id).next_due_us, first_at);

    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal_id).status, AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    const ayni_goal_info_v1 goal = work.goal(goal_id);
    EXPECT_EQ(goal.occurrences, 3U);
    EXPECT_EQ(goal.next_due_us, no_time);
    const ayni_task_info_v1 task = work.task(goal.last_task_id);
    EXPECT_EQ(task.occurrence, 3U);
    EXPECT_EQ(task.occurrence_count, 3U);
    EXPECT_EQ(task.due_us, last_at);
}

// A terminal goal drops the inputs it has not taken and takes no more.
TEST(WorkInput, terminal_goals_take_nothing) {
    WorkNode work;
    work.run();
    const uint64_t goal_id = input_goal(work, "button");
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal_id).status, AYNI_OK);
    ASSERT_EQ(post(work), AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    ASSERT_NE(work.goal(goal_id).next_due_us, no_time);

    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_CANCEL, goal_id).status, AYNI_OK);
    const ayni_goal_info_v1 cancelled = work.goal(goal_id);
    EXPECT_EQ(cancelled.next_due_us, no_time);
    EXPECT_EQ(cancelled.wait_reason, AYNI_REASON_NONE);
    EXPECT_EQ(work.node().step().items_done, 0U);
    ASSERT_EQ(post(work), AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.node().info().events_unmatched, 1U);
    const ayni_goal_info_v1 after = work.goal(goal_id);
    EXPECT_EQ(after.occurrences, 0U);
    EXPECT_EQ(after.last_task_id, 0U);
    EXPECT_EQ(after.commit_seq, cancelled.commit_seq);
}

// C-RES-005, ADR-0020: while the task table has no room, recorded inputs
// wait for the goal type's limit, counted from the earliest; then all of
// them are skipped together, the revision staying.
TEST(WorkInput, inputs_waiting_too_long_are_skipped) {
    WorkNode work;
    work.timer_wait_max_us = 2U * second;
    work.node().config().task_capacity = 1;
    work.run();
    ayni_goal_create_v1 holder = WorkNode::create_body(work.instance(agent_b), 0, "reminder", 1,
                                                       {nullptr, 0}, AYNI_TRIGGER_ONCE);
    ASSERT_EQ(work.create(holder).status, AYNI_OK);
    ASSERT_EQ(work.node().step().items_done, 1U);
    // The strategy holds the task, so it keeps its place.
    work.strategy().answer = cannot_decide;
    ASSERT_EQ(work.node().step().items_done, 1U);
    const uint64_t goal_id = input_goal(work, "button");
    EXPECT_EQ(work.goal(goal_id).wait_reason, AYNI_REASON_NONE); // none to come yet

    const uint64_t first_at = now(work);
    ASSERT_EQ(post(work), AYNI_OK);
    ayni_step_report_v1 report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, first_at + 2U * second);
    const ayni_goal_info_v1 waiting = work.goal(goal_id);
    EXPECT_EQ(waiting.next_due_us, first_at);
    EXPECT_EQ(waiting.wait_reason, AYNI_REASON_TASK_CAPACITY);

    advance(work, second);
    ASSERT_EQ(post(work), AYNI_OK);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, first_at + 2U * second);

    advance(work, second);
    report = work.node().step();
    EXPECT_EQ(report.items_done, 1U);
    EXPECT_EQ(report.next_deadline_us, no_time);
    const ayni_goal_info_v1 skipped = work.goal(goal_id);
    EXPECT_EQ(skipped.occurrences, 2U);
    EXPECT_EQ(skipped.occurrences_skipped, 2U);
    EXPECT_EQ(skipped.skip_reason, AYNI_REASON_TASK_CAPACITY);
    EXPECT_EQ(skipped.wait_reason, AYNI_REASON_NONE);
    EXPECT_EQ(skipped.last_task_id, 0U);
    EXPECT_EQ(skipped.next_due_us, no_time);
    EXPECT_EQ(skipped.revision, waiting.revision);
}

// Each queue entry keeps room for a source, however small event bodies are
// configured, so the queue costs the same at every body size up to an id.
TEST(WorkInput, small_event_bodies_still_hold_a_source) {
    const char *longest = "abcdefghijklmnopqrstuvwxyz.0123_";
    ASSERT_EQ(std::strlen(longest), AYNI_CATALOG_ID_MAX_BYTES);
    WorkNode work;
    work.node().config().event_body_max_bytes = 1;
    work.run();
    const uint64_t goal_id = input_goal(work, longest);
    ASSERT_EQ(post(work, longest), AYNI_OK);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.node().step().items_done, 1U);
    EXPECT_EQ(work.goal(goal_id).occurrences, 1U);

    ayni_config_v1 config = work.node().config();
    std::size_t align = 0;
    std::size_t at_1 = 0;
    std::size_t at_id = 0;
    std::size_t at_64 = 0;
    ASSERT_EQ(ayni_kernel_memory_requirements(&config, &at_1, &align), AYNI_OK);
    config.event_body_max_bytes = AYNI_CATALOG_ID_MAX_BYTES;
    ASSERT_EQ(ayni_kernel_memory_requirements(&config, &at_id, &align), AYNI_OK);
    config.event_body_max_bytes = 64;
    ASSERT_EQ(ayni_kernel_memory_requirements(&config, &at_64, &align), AYNI_OK);
    EXPECT_EQ(at_1, at_id);
    EXPECT_GT(at_64, at_id);
}

// ADR-0018 decision 4: a goal type takes only the trigger kinds it lists.
TEST(WorkInput, goal_type_must_accept_local_input) {
    WorkNode work;
    work.run();
    expect_refused(work.create(input_body(work, text("button"), "folder")), AYNI_INVALID_ARGUMENT,
                   AYNI_REASON_TRIGGER_KIND, AYNI_OBJECT_GOAL_TYPE, 0);
}

} // namespace
