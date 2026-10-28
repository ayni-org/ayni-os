// ADR-0063/C-SUB-001..004: normal subscriptions through public callbacks and queries.
#include "subscription_fixture.h"
namespace {
using namespace ayni::test;

// ADR-0063 / C-SUB-001/003: the Mock owner receives an actual Kernel borrow
// and sink. Rejected startup/renew inputs must neither retain it nor alter lease state.
TEST_F(SubscriptionFixture, mock_capture_rejects_invalid_openings_and_renewals_atomically) {
    uint32_t inspected = 0;
    inspect_camera_submit = [&](const ayni_provider_request_v1 *original,
                                ayni_provider_completion_t *completion) {
        ++inspected;
        auto request = *original;
        EXPECT_EQ(camera_ports.submit(camera_ports.ctx, nullptr, completion),
                  AYNI_SUBMIT_REJECTED_NOT_RETAINED);
        EXPECT_EQ(camera_ports.submit(camera_ports.ctx, original, nullptr),
                  AYNI_SUBMIT_REJECTED_NOT_RETAINED);
        request.subscription = nullptr;
        EXPECT_EQ(camera_ports.submit(camera_ports.ctx, &request, completion),
                  AYNI_SUBMIT_REJECTED_NOT_RETAINED);
        const auto invalid_openings = {
            +[](ayni_subscription_open_v1 &v) { v.struct_size = 0; },
            +[](ayni_subscription_open_v1 &v) { v.sink = nullptr; },
            +[](ayni_subscription_open_v1 &v) { v.valid_until_us = 0; },
            +[](ayni_subscription_open_v1 &v) { v.subscription.boot_epoch = 0; },
        };
        for (const auto change : invalid_openings) {
            auto invalid = *original->subscription;
            change(invalid);
            request.subscription = &invalid;
            EXPECT_EQ(camera_ports.submit(camera_ports.ctx, &request, completion),
                      AYNI_SUBMIT_REJECTED_NOT_RETAINED);
            EXPECT_EQ(camera.pending(), 0U);
        }
        // The fixture forwards the unchanged original while all borrowed views live.
    };
    create();
    pump(15, 10000, false);
    inspect_camera_submit = {};
    ASSERT_EQ(inspected, 1U);
    ASSERT_EQ(openings.size(), 1U);
    const auto opening = openings.front();
    EXPECT_EQ(camera.close(), AYNI_BUSY);
    EXPECT_EQ(camera.pending(), 1U);
    EXPECT_LE(camera.next_due_us(), work.node().sim().now_us());
    ayni_subscription_renew_v1 renewal{};
    renewal.struct_size = sizeof(renewal);
    renewal.subscription = opening.value.subscription;
    renewal.sequence = 2;
    renewal.valid_until_us = opening.value.valid_until_us + 1;
    EXPECT_EQ(camera_ports.renew(camera_ports.ctx, &renewal), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    pump(3);
    ASSERT_EQ(subscription().phase, AYNI_SUBSCRIPTION_ACTIVE);
    EXPECT_EQ(camera_ports.renew(camera_ports.ctx, nullptr), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    const auto invalid_renewals = {
        +[](ayni_subscription_renew_v1 &v) { v.struct_size = 0; },
        +[](ayni_subscription_renew_v1 &v) { v.reserved = 1; },
        +[](ayni_subscription_renew_v1 &v) { ++v.subscription.subscription_id; },
        +[](ayni_subscription_renew_v1 &v) { ++v.subscription.boot_epoch; },
        +[](ayni_subscription_renew_v1 &v) { ++v.subscription.generation; },
        +[](ayni_subscription_renew_v1 &v) { --v.sequence; },
        +[](ayni_subscription_renew_v1 &v) { ++v.sequence; },
        +[](ayni_subscription_renew_v1 &v) { --v.valid_until_us; },
        +[](ayni_subscription_renew_v1 &v) { v.valid_until_us = UINT64_MAX; },
    };
    for (const auto change : invalid_renewals) {
        auto invalid = renewal;
        change(invalid);
        EXPECT_EQ(camera_ports.renew(camera_ports.ctx, &invalid),
                  AYNI_SUBMIT_REJECTED_NOT_RETAINED);
        EXPECT_EQ(camera.pending(), 1U);
        EXPECT_EQ(subscription().lease_sequence, 1U);
    }
    auto absent = opening.attempt;
    ++absent.boot_epoch;
    EXPECT_EQ(camera_ports.cancel(camera_ports.ctx, absent), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    absent = opening.attempt;
    ++absent.request_id;
    EXPECT_EQ(camera_ports.cancel(camera_ports.ctx, absent), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
}

TEST_F(SubscriptionFixture, start_ack_precedes_samples_and_active_collection_allows_model_call) {
    const auto goal = create();
    pump(15, 10000, false);
    ASSERT_EQ(openings.size(), 1U);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_STARTING);
    EXPECT_EQ(model.pending(), 0U);
    ayni_observation_info_v1 absent{};
    absent.struct_size = sizeof(absent);
    EXPECT_EQ(ayni_get_subscription_observation(
                  work.kernel(), subscription().subscription.subscription_id, &absent),
              AYNI_NOT_FOUND);
    const auto &opening = openings.front().value;
    const ayni_observation_v1 sample{sizeof(sample),
                                     1,
                                     text("mock.camera"),
                                     opening.subscription.generation,
                                     1,
                                     work.node().sim().now_us(),
                                     50000,
                                     AYNI_OBSERVATION_GOOD,
                                     0,
                                     1};
    const ayni_subscription_observation_v1 input{sizeof(input), 0, opening.subscription, sample,
                                                 nullptr};
    EXPECT_EQ(ayni_subscription_post_observation(opening.sink, &input), AYNI_NOT_READY);
    ASSERT_EQ(camera.advance(), AYNI_OK);
    EXPECT_EQ(ayni_subscription_post_observation(opening.sink, &input), AYNI_NOT_READY);
    pump(6);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_ACTIVE);
    EXPECT_EQ(model.pending(), 1U);
    EXPECT_EQ(work.task(task(goal)).lifecycle, AYNI_TASK_RUNNING);
    EXPECT_EQ(observation().reference.subscription_id, opening.subscription.subscription_id);
    EXPECT_EQ(observation().usable, 1U);
    EXPECT_EQ(work.node().sim().calls_under_lock(), 0U);
}
TEST_F(SubscriptionFixture, half_lease_renewal_confirms_the_requested_absolute_deadline) {
    create();
    pump(20);
    ASSERT_EQ(openings.size(), 1U);
    const auto initial = subscription();
    EXPECT_EQ(initial.valid_until_us - openings.front().value.valid_until_us, 0U);
    work.node().sim().advance_us(15000000);
    pump(2, 0, false);
    ASSERT_EQ(renewals.size(), 1U);
    EXPECT_EQ(subscription().valid_until_us, initial.valid_until_us);
    const auto requested = renewals.front().valid_until_us;
    EXPECT_GT(requested, initial.valid_until_us);
    work.node().sim().advance_us(1000000);
    pump(3, 0);
    EXPECT_EQ(subscription().lease_sequence, 2U);
    EXPECT_EQ(subscription().valid_until_us, requested);
    EXPECT_EQ(subscription().subscription.generation, initial.subscription.generation);
}
// ADR-0065: a group gate uses the same immediate stop boundary and new-generation reopen.
TEST_F(SubscriptionFixture, group_pause_closes_capture_before_returning_and_reopens_fresh) {
    create(agent_a, 1);
    pump(20);
    const auto before = subscription();
    ASSERT_EQ(observation().usable, 1U);
    ayni_pause_group_set_v1 body{sizeof(body), 1, 1, 0};
    auto intent = work.intent(AYNI_INTENT_PAUSE_GROUP_SET, &body, 0);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    EXPECT_EQ(observation().usable, 0U);
    pump(8);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_STOPPED);
    EXPECT_EQ(camera.pending(), 0U);
    body.paused = 0;
    intent.expected_revision = 1;
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    pump(20);
    ASSERT_EQ(openings.size(), 2U);
    EXPECT_EQ(subscription().subscription.subscription_id, before.subscription.subscription_id);
    EXPECT_GT(subscription().subscription.generation, before.subscription.generation);
    EXPECT_EQ(observation().usable, 1U);
}

TEST_F(SubscriptionFixture, task_pause_invalidates_cache_and_resume_opens_a_new_generation) {
    const auto goal = create();
    pump(20);
    const auto before = subscription();
    EXPECT_EQ(observation().usable, 1U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    EXPECT_EQ(observation().usable, 0U);
    pump(8);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_STOPPED);
    EXPECT_EQ(camera.pending(), 0U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal).status, AYNI_OK);
    pump(20);
    ASSERT_EQ(openings.size(), 2U);
    EXPECT_EQ(subscription().subscription.subscription_id, before.subscription.subscription_id);
    EXPECT_GT(subscription().subscription.generation, before.subscription.generation);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_ACTIVE);
    EXPECT_EQ(observation().reference.generation, subscription().subscription.generation);
}

// C-SUB-002 / ADR-0063: known stop permits a fresh collection generation.
// The journal must restore that generation without treating it as an old query.
TEST_F(SubscriptionFixture, restart_replays_a_generation_previously_reopened_after_known_stop) {
    const auto goal = create();
    pump(20);
    const auto first = subscription();
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    pump(8);
    ASSERT_EQ(subscription().phase, AYNI_SUBSCRIPTION_STOPPED);
    ASSERT_EQ(camera.pending(), 0U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal).status, AYNI_OK);
    pump(20);
    const auto reopened = subscription();
    ASSERT_EQ(reopened.phase, AYNI_SUBSCRIPTION_ACTIVE);
    ASSERT_GT(reopened.subscription.generation, first.subscription.generation);
    shutdown();
    ASSERT_EQ(work.node().stop_and_destroy(), AYNI_OK);
    work.node().create_and_start();
    work.node().recover();
    ASSERT_EQ(work.node().lifecycle(), AYNI_LIFECYCLE_RUNNING);
    work.reopen_session();
    pump(25);
    const auto recovered = subscription();
    EXPECT_EQ(recovered.subscription.subscription_id, first.subscription.subscription_id);
    EXPECT_EQ(recovered.task_id, first.task_id);
    EXPECT_GT(recovered.subscription.generation, reopened.subscription.generation);
    EXPECT_EQ(recovered.phase, AYNI_SUBSCRIPTION_ACTIVE);
    EXPECT_EQ(work.goal(goal).last_task_id, first.task_id);
}
TEST_F(SubscriptionFixture, two_tasks_share_capacity_but_keep_samples_media_and_close_isolated) {
    create(agent_a);
    create(agent_b);
    pump(25);
    ASSERT_EQ(openings.size(), 2U);
    const auto a = observation(0);
    const auto b = observation(1);
    EXPECT_NE(a.reference.subscription_id, b.reference.subscription_id);
    EXPECT_NE(a.reference.generation, b.reference.generation);
    EXPECT_EQ(a.usable, 1U);
    EXPECT_EQ(b.usable, 1U);
    EXPECT_EQ(camera_resource().used, 2U);
    const auto closed = openings.front().value;
    ASSERT_EQ(work.cancel_task(subscription().task_id).status, AYNI_OK);
    pump(8);
    EXPECT_EQ(observation(0).usable, 0U);
    EXPECT_EQ(observation(1).usable, 1U);
    EXPECT_EQ(camera_resource().used, 1U);
    uint8_t bytes[3]{};
    uint32_t size = 0;
    const auto remaining = observation(1).media;
    EXPECT_EQ(camera.media_store()->read(remaining, remaining.owner_instance_id, text("inspect"),
                                         work.node().sim().now_us(), bytes, sizeof(bytes), size),
              AYNI_OK);
    EXPECT_EQ(ayni_subscription_report_gap(closed.sink, closed.subscription, UINT64_MAX),
              AYNI_NOT_READY);
}
TEST_F(SubscriptionFixture, explicit_strategy_close_releases_its_own_subscription_after_terminal) {
    const auto goal = create();
    pump(20);
    finish_analysis = true;
    pump(10, 10000, false, true);
    EXPECT_TRUE(close_requested);
    EXPECT_EQ(camera_resource().used, 1U);
    pump(10);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_STOPPED);
    EXPECT_EQ(camera_resource().used, 0U);
    EXPECT_EQ(work.task(task(goal)).lifecycle, AYNI_TASK_COMPLETED);
}
TEST_F(SubscriptionFixture, orderly_restart_preserves_intent_and_rechecks_a_fresh_opening) {
    const auto goal = create();
    pump(20);
    const auto before = subscription();
    shutdown();
    ASSERT_EQ(work.node().stop_and_destroy(), AYNI_OK);
    work.node().create_and_start();
    work.node().recover();
    work.reopen_session();
    pump(25);
    EXPECT_EQ(work.goal(goal).last_task_id, before.task_id);
    EXPECT_EQ(subscription().task_id, before.task_id);
    EXPECT_GT(subscription().subscription.generation, before.subscription.generation);
    EXPECT_GT(subscription().subscription.boot_epoch, before.subscription.boot_epoch);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_ACTIVE);
}
// C-SUB-005: an idle observer has no repeated decide calls or revision growth.
TEST_F(SubscriptionFixture, waiting_task_wakes_once_for_the_latest_sample_and_subscription_state) {
    std::vector<uint64_t> seen;
    uint32_t decisions = 0;
    work.strategy().answer = [&](const auto &snapshot, auto &output) {
        ++decisions;
        ayni_subscription_info_v1 info{};
        info.struct_size = sizeof(info);
        if (snapshot.subscription(snapshot.call_context, text("watch"), &info) == AYNI_NOT_FOUND) {
            return request(snapshot, output, "watch", "camera", AYNI_POLICY_REQUEST_CALL);
        }
        ayni_observation_info_v1 sample{};
        sample.struct_size = sizeof(sample);
        if (snapshot.subscription_observation(snapshot.call_context, text("watch"), &sample) ==
            AYNI_OK) {
            seen.push_back(sample.reference.sequence);
        }
        return request(snapshot, output, "watch", nullptr, AYNI_POLICY_WAIT_SUBSCRIPTION);
    };
    const auto goal = create();
    pump(20);
    ASSERT_EQ(work.task(task(goal)).lifecycle, AYNI_TASK_WAITING);
    pump(6, 0, false); // Drain the last accepted frame before measuring idle work.
    const auto before = work.task(task(goal));
    const auto decided = decisions;
    pump(10, 0, false);
    EXPECT_EQ(decisions, decided);
    EXPECT_EQ(work.task(task(goal)).revision, before.revision);
    EXPECT_EQ(work.node().step().more_ready, 0U);
    ASSERT_FALSE(seen.empty());
    const auto old = seen.back();
    pump(6, 10000);
    EXPECT_GT(seen.back(), old);
    EXPECT_EQ(work.task(task(goal)).wait_sequence, seen.back());
    const auto first_generation = subscription().subscription.generation;
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    pump(8);
    EXPECT_EQ(work.task(task(goal)).lifecycle, AYNI_TASK_PAUSED);
    EXPECT_NE(work.task(task(goal)).wait_subscription_id, 0U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal).status, AYNI_OK);
    pump(20);
    EXPECT_GT(subscription().subscription.generation, first_generation);
    EXPECT_EQ(work.task(task(goal)).lifecycle, AYNI_TASK_WAITING);
    EXPECT_EQ(work.task(task(goal)).wait_generation, subscription().subscription.generation);
}
// C-SUB-005: collection remains independent of model work; no per-frame backlog.
TEST_F(SubscriptionFixture, model_busy_consumer_resumes_with_the_latest_sample_then_waits) {
    std::vector<uint64_t> seen;
    bool requested = false;
    work.strategy().answer = [&](const auto &snapshot, auto &output) {
        ayni_subscription_info_v1 info{};
        info.struct_size = sizeof(info);
        if (snapshot.subscription(snapshot.call_context, text("watch"), &info) == AYNI_NOT_FOUND) {
            return request(snapshot, output, "watch", "camera", AYNI_POLICY_REQUEST_CALL);
        }
        ayni_observation_info_v1 sample{};
        sample.struct_size = sizeof(sample);
        if (snapshot.subscription_observation(snapshot.call_context, text("watch"), &sample) ==
            AYNI_OK) {
            seen.push_back(sample.reference.sequence);
        }
        if (!requested && sample.usable) {
            requested = true;
            return request(snapshot, output, "analysis", "model", AYNI_POLICY_REQUEST_MODEL);
        }
        return request(snapshot, output, "watch", nullptr, AYNI_POLICY_WAIT_SUBSCRIPTION);
    };
    const auto goal = create();
    pump(20);
    ASSERT_TRUE(requested);
    ASSERT_EQ(model.pending(), 1U);
    const auto rounds = seen.size();
    pump(10);
    EXPECT_EQ(seen.size(), rounds);
    const auto latest = observation().reference.sequence;
    ASSERT_GT(latest, seen.back());
    pump(5, 0, false, true);
    EXPECT_EQ(seen.back(), latest);
    EXPECT_EQ(work.task(task(goal)).lifecycle, AYNI_TASK_WAITING);
    const auto revision = work.task(task(goal)).revision;
    pump(5, 0, false);
    EXPECT_EQ(work.task(task(goal)).revision, revision);
}
// C-SUB-004/005: orderly reopen retains the Task's wait, then new-generation
// state wakes it; no sample bytes or boot-local timestamps are restored.
TEST_F(SubscriptionFixture,
       orderly_restart_of_waiting_task_retains_frontier_and_follows_new_generation) {
    uint32_t decisions = 0;
    work.strategy().answer = [&](const auto &snapshot, auto &output) {
        ++decisions;
        ayni_subscription_info_v1 info{};
        info.struct_size = sizeof(info);
        if (snapshot.subscription(snapshot.call_context, text("watch"), &info) == AYNI_NOT_FOUND) {
            return request(snapshot, output, "watch", "camera", AYNI_POLICY_REQUEST_CALL);
        }
        return request(snapshot, output, "watch", nullptr, AYNI_POLICY_WAIT_SUBSCRIPTION);
    };
    const auto goal = create();
    pump(20);
    const auto before = work.task(task(goal));
    ASSERT_EQ(before.lifecycle, AYNI_TASK_WAITING);
    shutdown();
    ASSERT_EQ(work.node().stop_and_destroy(), AYNI_OK);
    work.node().create_and_start();
    work.node().recover();
    work.reopen_session();
    const auto decided = decisions;
    pump(20);
    EXPECT_EQ(work.task(task(goal)).lifecycle, AYNI_TASK_WAITING);
    EXPECT_EQ(work.task(task(goal)).wait_subscription_id, before.wait_subscription_id);
    EXPECT_GT(work.task(task(goal)).wait_generation, before.wait_generation);
    EXPECT_GT(decisions, decided);
}
// C-SUB-002/004: a gap invalidates the scoped fact until a strictly newer sample.
TEST_F(SubscriptionFixture,
       gap_closes_the_cached_frontier_and_old_opening_cannot_publish_after_resume) {
    const auto goal = create();
    pump(20);
    const auto old = openings.front().value;
    auto last = observation();
    ASSERT_EQ(ayni_subscription_report_gap(old.sink, old.subscription, last.reference.sequence),
              AYNI_OK);
    pump(2, 0, false);
    EXPECT_EQ(observation().usable, 0U);
    const ayni_observation_v1 sample{sizeof(sample),
                                     1,
                                     text("mock.camera"),
                                     old.subscription.generation,
                                     last.reference.sequence + 1,
                                     work.node().sim().now_us(),
                                     50000,
                                     AYNI_OBSERVATION_GOOD,
                                     0,
                                     1};
    const ayni_subscription_observation_v1 input{sizeof(input), 0, old.subscription, sample,
                                                 nullptr};
    ASSERT_EQ(ayni_subscription_post_observation(old.sink, &input), AYNI_OK);
    pump(2, 0, false);
    EXPECT_EQ(observation().usable, 1U);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    pump(8);
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_RESUME, goal).status, AYNI_OK);
    pump(20);
    ASSERT_EQ(openings.size(), 2U);
    EXPECT_EQ(ayni_subscription_post_observation(old.sink, &input), AYNI_STALE);
    EXPECT_NE(observation().reference.generation, old.subscription.generation);
}
// C-SUB-003: revocation closes all dependent consumption before Provider polling.
TEST_F(SubscriptionFixture,
       revoking_capture_authority_immediately_invalidates_then_stops_collection) {
    create();
    pump(20);
    ASSERT_EQ(observation().usable, 1U);
    const auto opening = openings.front().value;
    const ayni_grant_revoke_v1 body{sizeof(body), 0, grants.front()};
    auto intent = work.intent(AYNI_INTENT_GRANT_REVOKE, &body, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    EXPECT_EQ(observation().usable, 0U);
    EXPECT_EQ(ayni_subscription_report_gap(opening.sink, opening.subscription, UINT64_MAX),
              AYNI_NOT_READY);
    pump(10);
    EXPECT_EQ(camera.pending(), 0U);
    EXPECT_EQ(camera_resource().used, 0U);
    EXPECT_TRUE(renewals.empty());
}
class SubscriptionQueryFixture : public SubscriptionFixture {
    void SetUp() override {
        manual_camera = true;
        query_contract = true;
        SubscriptionFixture::SetUp();
    }
};
// C-SUB-003/C-CALL-010: terminal UNKNOWN returns transport, not collection-stop
// proof. A management query waits safely for the finite retained lease owner.
TEST_F(SubscriptionQueryFixture,
       unknown_retains_units_and_query_waits_until_the_longest_requested_lease) {
    create();
    pump(15);
    ASSERT_EQ(openings.size(), 1U);
    post(openings.front(), AYNI_CALL_SUBSCRIPTION_STARTED, 1);
    pump(5);
    const auto initial = subscription();
    work.node().sim().advance_us(15000000);
    pump(2, 0);
    ASSERT_EQ(renewals.size(), 1U);
    const auto longest = renewals.front().valid_until_us;
    post(openings.front(), AYNI_CALL_ABANDONED);
    pump(4, 0);
    EXPECT_EQ(camera_resource().used, 1U);
    const ayni_call_query_result_v1 body{sizeof(body), 0, initial.subscription.subscription_id};
    auto intent = work.intent(AYNI_INTENT_CALL_QUERY_RESULT, &body, 1);
    intent.command_id = ++command;
    ASSERT_EQ(work.submit(intent).status, AYNI_OK);
    pump(4);
    EXPECT_TRUE(queries.empty());
    work.node().sim().advance_us(initial.valid_until_us - work.node().sim().now_us());
    pump(2, 0);
    EXPECT_EQ(camera_resource().used, 1U);
    work.node().sim().advance_us(longest - work.node().sim().now_us());
    pump(8, 0);
    EXPECT_EQ(camera_resource().used, 0U);
    ASSERT_EQ(queries.size(), 1U);
    EXPECT_EQ(queries.front().operation_id, initial.subscription.generation);
}
// C-SUB-003: a late start ACK cannot extend or revive an expired finite lease.
TEST_F(SubscriptionQueryFixture,
       expired_start_lease_rejects_samples_and_late_ack_does_not_reactivate) {
    create();
    pump(15);
    ASSERT_EQ(openings.size(), 1U);
    const auto opening = openings.front().value;
    work.node().sim().advance_us(opening.valid_until_us - work.node().sim().now_us());
    post(openings.front(), AYNI_CALL_SUBSCRIPTION_STARTED, 1);
    pump(4, 0);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_EXPIRED);
    EXPECT_EQ(subscription().lease_sequence, 0U);
    EXPECT_EQ(ayni_subscription_report_gap(opening.sink, opening.subscription, 0), AYNI_NOT_READY);
    EXPECT_EQ(camera_resource().used, 1U); // Expiry is not return of the borrowed completion.
    post(openings.front(), AYNI_CALL_CANCELLED);
    pump(5, 0);
    EXPECT_EQ(camera_resource().used, 0U);
    EXPECT_EQ(subscription().phase, AYNI_SUBSCRIPTION_EXPIRED);
}
class SubscriptionBackpressureFixture : public SubscriptionFixture {
    void SetUp() override {
        manual_camera = true;
        minimal_journal = true;
        SubscriptionFixture::SetUp();
    }
};
// C-SUB-003: independent close does not erase the domain stop that still needs
// a journal row once ordinary asynchronous storage pressure clears.
TEST_F(SubscriptionBackpressureFixture,
       one_completion_item_reports_the_stop_still_ready_after_cancel_was_sent) {
    const auto goal = create();
    pump(15);
    ASSERT_EQ(openings.size(), 1U);
    post(openings.front(), AYNI_CALL_SUBSCRIPTION_STARTED, 1);
    for (uint32_t i = 0; i < 12 && subscription().phase != AYNI_SUBSCRIPTION_ACTIVE; ++i) {
        work.node().sim().advance_us(10000);
        work.node().step(1);
        work.node().sim().complete_storage();
    }
    ASSERT_EQ(subscription().phase, AYNI_SUBSCRIPTION_ACTIVE);
    ASSERT_EQ(model.pending(), 0U);
    work.node().step(1); // Only the ACK batch storage completion, with no decision budget left.
    ASSERT_EQ(work.change(AYNI_INTENT_GOAL_PAUSE, goal).status, AYNI_OK);
    work.node().sim().advance_us(AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT);
    work.node().step(); // Journal is full; only the independent cancellation can run.
    work.node().sim().complete_storage();
    const auto completed = work.node().step(1);
    EXPECT_EQ(completed.items_done, 1U);
    EXPECT_EQ(completed.more_ready, 1U);
    EXPECT_EQ(camera_resource().used, 1U);
}
} // namespace
