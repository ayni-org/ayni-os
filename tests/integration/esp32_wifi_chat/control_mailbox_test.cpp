#include "../../support/latency_samples.h"
#include "../wifi/motor_panel_text.h"
#include "control_mailbox.h"
#include "transport.h"
#include <atomic>
#include <ayni/control_loop.h>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace {
using namespace wifichat;

TEST(ControlMailbox, LatestTargetPreservesIndependentReceiptSupersessionAndExecutionFrontiers) {
    // ADR-0114: receipt, supersession, application and rejection are distinct facts.
    ControlMailbox mailbox;
    uint64_t first = 0, second = 0;
    ASSERT_TRUE(mailbox.submit({0, 0, 30, -30, 100}, 10, &first));
    ASSERT_TRUE(mailbox.submit({0, 0, 40, -40, 200}, 20, &second));
    auto feedback = mailbox.snapshot();
    EXPECT_EQ(feedback.accepted_seq, second);
    EXPECT_EQ(feedback.superseded_seq, first);
    EXPECT_EQ(feedback.superseded_count, 1U);
    EXPECT_EQ(feedback.applied_seq, 0U);
    ControlTarget target;
    ASSERT_TRUE(mailbox.take_target(target));
    EXPECT_EQ(target.seq, second);
    EXPECT_EQ(target.motor1, 40);
    EXPECT_FALSE(mailbox.take_target(target));
    mailbox.mark_rejected(second, ControlRejectReason::no_active_session, AYNI_DENIED, 30);
    feedback = mailbox.snapshot();
    EXPECT_EQ(feedback.rejected_seq, second);
    EXPECT_EQ(feedback.reject_reason, ControlRejectReason::no_active_session);
    mailbox.mark_applied(second, 20, -20, 40);
    feedback = mailbox.snapshot();
    EXPECT_EQ(feedback.applied_seq, second);
    EXPECT_EQ(feedback.applied_motor1, 20);
    EXPECT_EQ(feedback.applied_at_us, 40U);
}

TEST(ControlMailbox, StopSurvivesPollingAndEarlierConfirmationCannotClearNewerOrDowngradeHardStop) {
    // ADR-0114: actual stop ACK is the only clear; all pre-stop targets remain fenced afterward.
    ControlMailbox mailbox;
    uint64_t before = 0, after = 0;
    ASSERT_TRUE(mailbox.submit({0, 0, 40, 40, 200}, 1, &before));
    const auto soft = mailbox.request_stop(ControlStopKind::soft_stop);
    for (unsigned at = 0; at < 100; ++at) {
        EXPECT_TRUE(mailbox.stop_pending());
    }
    const auto hard = mailbox.request_stop(ControlStopKind::hard_estop);
    mailbox.confirm_stop(soft, ControlMode::armed, 0, 2);
    EXPECT_TRUE(mailbox.stop_pending());
    const auto again = mailbox.request_stop(ControlStopKind::soft_stop);
    EXPECT_EQ(mailbox.stop_request().kind, ControlStopKind::hard_estop);
    EXPECT_EQ(again.kind, ControlStopKind::hard_estop);
    EXPECT_GT(again.seq, hard.seq);
    mailbox.confirm_stop(hard, ControlMode::latched, 0, 3);
    EXPECT_TRUE(mailbox.stop_pending());
    mailbox.confirm_stop(again, ControlMode::latched, 0, 4);
    EXPECT_FALSE(mailbox.stop_pending());
    EXPECT_TRUE(mailbox.target_blocked(before));
    ASSERT_TRUE(mailbox.submit({0, 0, 10, 10, 100}, 5, &after));
    EXPECT_TRUE(mailbox.target_blocked(after)); // Hard E-stop remains latched until reboot.
    EXPECT_EQ(mailbox.snapshot().stop_confirmed_seq, again.seq);
}

TEST(ControlMailbox, ConcurrentProducersExecutorAndUiNeverShareMutablePayloadSlots) {
    // ADR-0114 / C++17: exercise ownership and latest-wins accounting under normal contention.
    ControlMailbox mailbox;
    constexpr uint32_t samples = 10000;
    std::atomic<uint32_t> producers_done{0};
    std::atomic<bool> consumer_done{false}, coherent{true};
    std::atomic<uint32_t> taken{0};
    auto producer = [&] {
        for (uint32_t at = 1; at <= samples; ++at) {
            ControlTarget target{0, 0, int32_t(at), -int32_t(at), at};
            while (!mailbox.submit(target, at)) {
                std::this_thread::yield();
            }
        }
        ++producers_done;
    };
    auto first = std::async(std::launch::async, producer);
    auto second = std::async(std::launch::async, producer);
    auto executor = std::async(std::launch::async, [&] {
        uint64_t previous = 0;
        for (;;) {
            ControlTarget target;
            if (mailbox.take_target(target)) {
                coherent = coherent && target.seq > previous && target.motor1 == -target.motor2 &&
                           uint32_t(target.motor1) == target.duration_ms;
                previous = target.seq;
                ++taken;
                mailbox.mark_applied(target.seq, target.motor1, target.motor2,
                                     target.accepted_at_us);
            } else if (producers_done.load() == 2) {
                break;
            }
        }
        consumer_done = true;
    });
    uint64_t accepted = 0, applied = 0;
    while (!consumer_done.load()) {
        const auto feedback = mailbox.snapshot();
        EXPECT_GE(feedback.accepted_seq, accepted);
        EXPECT_GE(feedback.applied_seq, applied);
        EXPECT_EQ(feedback.applied_motor1, -feedback.applied_motor2);
        accepted = feedback.accepted_seq;
        applied = feedback.applied_seq;
    }
    first.get();
    second.get();
    executor.get();
    EXPECT_TRUE(coherent.load());
    EXPECT_EQ(mailbox.snapshot().superseded_count + taken.load(), 2U * samples);
}

TEST(ControlMailbox, OutputSaturationLeavesAllInputSlotsAndModelLaneAvailable) {
    // ADR-0114: independent 24 KiB input / 64 KiB output payload capacity.
    ControlMailbox control;
    Transport transport{control};
    ASSERT_TRUE(transport.initialize());
    for (size_t at = 0; at < output_packet_slots; ++at) {
        ASSERT_TRUE(transport.send(Kind::answer, at, nullptr, 0));
    }
    EXPECT_FALSE(transport.send(Kind::answer, 100, nullptr, 0));
    const Kind inputs[]{Kind::cancel,      Kind::motor_heartbeat, Kind::user,
                        Kind::display_ack, Kind::configure,       Kind::render_info};
    static_assert(sizeof(inputs) / sizeof(inputs[0]) == input_packet_slots);
    for (const auto kind : inputs) {
        ASSERT_TRUE(transport.post(kind, 0, nullptr, 0));
    }
    Packet packet;
    for (const auto kind : inputs) {
        ASSERT_TRUE(transport.receive(packet));
        EXPECT_EQ(packet.kind, kind);
    }
    EXPECT_FALSE(transport.receive(packet));
    EXPECT_FALSE(transport.post(Kind::user, 0, &packet, input_bytes + 1));
    EXPECT_FALSE(transport.send(Kind::answer, 0, &packet, summary_bytes + 1));
}
TEST(ControlMailbox, QueuedStartDoesNotEnterHotTargetLaneAndStopBarrierStillRejectsIt) {
    // ADR-0114: starts have one ordered admission lane; a confirmed stop still fences old starts.
    ControlMailbox mailbox;
    const auto start = mailbox.reserve_start();
    ControlTarget target;
    EXPECT_FALSE(mailbox.take_target(target));
    const auto stop = mailbox.request_stop(ControlStopKind::soft_stop);
    mailbox.confirm_stop(stop, ControlMode::armed, 0, 5);
    EXPECT_FALSE(mailbox.stop_pending());
    EXPECT_TRUE(mailbox.target_blocked(start));
    const auto fresh = mailbox.reserve_start();
    EXPECT_FALSE(mailbox.target_blocked(fresh));
}
TEST(ControlMailbox, BargeInDefaultsToHoldAndExplicitCancelStillStops) {
    // ADR-0114: policy applies only to natural language; explicit cancel retains soft-stop.
    ControlMailbox control;
    Transport transport(control);
    ASSERT_TRUE(transport.initialize());
    transport.apply_barge_in();
    EXPECT_EQ(control.hold().scale_percent, default_hold_scale_percent);
    ASSERT_TRUE(transport.set_barge_in_policy(BargeInPolicy::continue_motion));
    transport.apply_barge_in();
    EXPECT_EQ(control.hold().scale_percent, AYNI_CONTROL_LOOP_FULL_SCALE);
    EXPECT_FALSE(control.stop_pending());
    ASSERT_TRUE(transport.set_barge_in_policy(BargeInPolicy::stop));
    transport.apply_barge_in();
    EXPECT_TRUE(control.stop_pending());
    const auto stop = control.stop_request();
    control.confirm_stop(stop, ControlMode::armed, 0, 10);
    ASSERT_TRUE(transport.set_barge_in_policy(BargeInPolicy::continue_motion));
    transport.set_motor_active(true);
    transport.post(Kind::cancel, 0, nullptr, 0);
    EXPECT_TRUE(control.stop_pending());
    EXPECT_EQ(control.stop_request().kind, ControlStopKind::soft_stop);
}
TEST(ControlMailbox, TargetReceiptWallLatencyMeetsHostBudget) {
    // ADR-0114: real mailbox copies and receipt publication, without scheduler or hardware claims.
    ControlMailbox mailbox;
    ayni::test::LatencySamples samples;
    for (unsigned at = 0; at < 2000; ++at) {
        const auto start = ayni::test::LatencySamples::Clock::now();
        ASSERT_TRUE(mailbox.submit({0, 0, 40, -40, 100}, at));
        samples.add(start);
        ControlTarget target;
        ASSERT_TRUE(mailbox.take_target(target));
    }
    samples.print("ControlMailbox.submit");
    EXPECT_LT(samples.percentile(99), 50);
    EXPECT_LT(samples.percentile(100), 500);
}
TEST(ControlMailbox, NaturalInputAppliesPolicyOnceAfterInputCapacityReservation) {
    // ADR-0114: admission has one policy entry; a refused full-queue input cannot issue a hold.
    ControlMailbox control;
    Transport transport(control);
    ASSERT_TRUE(transport.initialize());
    transport.set_motor_active(true);
    for (unsigned at = 0; at < input_packet_slots; ++at) {
        ASSERT_TRUE(transport.post(Kind::user, at, "hello", 5));
        EXPECT_EQ(control.hold().seq, at + 1U);
    }
    EXPECT_FALSE(transport.post(Kind::user, 99, "hello", 5));
    EXPECT_EQ(control.hold().seq, input_packet_slots);
}
TEST(ControlMailbox, RecognitionCompletionCannotReapplyHoldAfterANewerStructuredTarget) {
    // ADR-0114: ASR capture and its later text are one instruction, with one control-policy event.
    ControlMailbox control;
    Transport transport(control);
    ASSERT_TRUE(transport.initialize());
    transport.set_motor_active(true);
    ASSERT_TRUE(transport.post(Kind::barge_in, 1, nullptr, 0));
    const auto hold = control.hold();
    uint64_t target_seq = 0;
    ASSERT_TRUE(transport.submit_motor_target({0, 0, 30, 30, 100}, &target_seq));
    ASSERT_TRUE(transport.post(Kind::recognized_user, 2, "hello", 5));
    EXPECT_EQ(control.hold().seq, hold.seq);
    EXPECT_GT(target_seq, hold.seq);
}
TEST(ControlMailbox, RunningSessionDoesNotPresentHistoricalStopAsCurrentFact) {
    // ADR-0114 / C-STOP-006: an old stop proof cannot describe a subsequently active session.
    ControlFeedbackSnapshot feedback;
    feedback.control_mode = uint8_t(ControlMode::armed);
    feedback.stop_requested_seq = feedback.stop_confirmed_seq = 1;
    EXPECT_STREQ(wifichat::wifi::motor_output_text(feedback), "输出已停止（软停，可恢复）");
    feedback.busy = 1;
    feedback.applied_seq = 2;
    feedback.applied_motor1 = 40;
    EXPECT_STREQ(wifichat::wifi::motor_output_text(feedback), "会话运行中");
    feedback.hold_active = 1;
    EXPECT_STREQ(wifichat::wifi::motor_output_text(feedback), "会话保持中");
    feedback.stop_requested_seq = 3;
    EXPECT_STREQ(wifichat::wifi::motor_output_text(feedback), "正在停机，等待硬件确认");
}
} // namespace

TEST(ControlMailbox, StopConfirmationCannotAcknowledgeANewerHardStopAsArmed) {
    // ADR-0114 / C-STOP-006: selected severity and hardware-confirmed mode must agree.
    ControlMailbox mailbox;
    const auto soft = mailbox.request_stop(ControlStopKind::soft_stop, 100);
    mailbox.confirm_stop(soft, ControlMode::armed, 0, 101);
    const auto stale = mailbox.stop_request();
    const auto hard = mailbox.request_stop(ControlStopKind::hard_estop, 102);
    mailbox.confirm_stop(stale, ControlMode::armed, 0, 103);
    EXPECT_TRUE(mailbox.stop_pending());
    EXPECT_EQ(mailbox.snapshot().stop_confirmed_seq, soft.seq);
    mailbox.confirm_stop(hard, ControlMode::armed, 0, 104);
    EXPECT_TRUE(mailbox.stop_pending());
    mailbox.confirm_stop(hard, ControlMode::latched, 0, 105);
    const auto feedback = mailbox.snapshot();
    EXPECT_EQ(feedback.control_mode, uint8_t(ControlMode::latched));
    EXPECT_EQ(feedback.accepted_at_us, 102U);
    EXPECT_FALSE(mailbox.stop_pending());
    EXPECT_TRUE(mailbox.target_blocked(mailbox.reserve_start()));
}
TEST(ControlMailbox, ApplicationTimestampIsFirstApplicationAndHoldReceiptHasTime) {
    // ADR-0114: steady sampling cannot drift command-to-application latency.
    ControlMailbox mailbox;
    mailbox.mark_applied(1, 20, 20, 100);
    mailbox.mark_applied(1, 20, 20, 200);
    mailbox.mark_applied(1, 5, 5, 300);
    EXPECT_EQ(mailbox.snapshot().applied_at_us, 100U);
    uint32_t seq = 0;
    ASSERT_TRUE(mailbox.request_hold(25, &seq, 350));
    EXPECT_EQ(mailbox.hold().seq, seq);
    EXPECT_EQ(mailbox.snapshot().accepted_at_us, 350U);
}
TEST(ControlMailbox, CommandSequenceContinuesPastTheFormer24BitLimit) {
    // ADR-0114: new commands, not control ticks, consume full-width sequence identities.
    ControlMailbox mailbox;
    constexpr uint32_t former_limit = UINT32_C(1) << 24U;
    for (uint32_t at = 1; at <= former_limit; ++at) {
        ASSERT_EQ(mailbox.reserve_start(), at);
    }
    EXPECT_FALSE(mailbox.stop_pending());
    EXPECT_EQ(mailbox.reserve_start(), former_limit + 1U);
}

TEST(ControlMailbox, ConcurrentHoldStopProducersHaveSeparateExecutorAndUiPayloadReaders) {
    // ADR-0114: multiwriter hold/stop telemetry must not share the executor's reader pin.
    ControlMailbox mailbox;
    std::atomic<unsigned> done{0};
    std::atomic<bool> finished{false}, coherent{true};
    auto produce = [&] {
        for (uint32_t at = 0; at < 2000; ++at) {
            const auto scale = at % 101U;
            while (!mailbox.request_hold(scale, nullptr, scale + 100U)) {
                std::this_thread::yield();
            }
            if (at % 11U == 0) {
                mailbox.request_stop(ControlStopKind::soft_stop, at + 1U);
            }
        }
        mailbox.request_stop(ControlStopKind::hard_estop, 10000);
        ++done;
    };
    auto first = std::async(std::launch::async, produce);
    auto second = std::async(std::launch::async, produce);
    auto executor = std::async(std::launch::async, [&] {
        while (done.load() != 2 || mailbox.stop_pending()) {
            const auto held = mailbox.hold();
            if (held.seq && held.accepted_at_us != held.scale_percent + 100U) {
                coherent = false;
            }
            if (const auto request = mailbox.pending_stop()) {
                mailbox.confirm_stop(*request,
                                     request->kind == ControlStopKind::hard_estop
                                         ? ControlMode::latched
                                         : ControlMode::armed,
                                     0, 20000);
            }
        }
        finished = true;
    });
    while (!finished.load()) {
        const auto feedback = mailbox.snapshot();
        if (feedback.accepted_kind == 2 &&
            (feedback.accepted_at_us < 100 || feedback.accepted_at_us > 200)) {
            coherent = false;
        }
    }
    first.get();
    second.get();
    executor.get();
    EXPECT_TRUE(coherent.load());
    EXPECT_FALSE(mailbox.stop_pending());
    EXPECT_TRUE(mailbox.hard_stop_requested());
    EXPECT_EQ(mailbox.snapshot().control_mode, uint8_t(ControlMode::latched));
}
