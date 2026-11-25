// ADR-0114 / C-STOP-006: production worker, real Gate/ControlSession/ControlLoop/mailbox.
// MotorOutput and task scheduling are replaced; real provider mailbox reservations retain
// terminal results. White-box peer selects races; no Core business commit or RMT I/O is claimed.
#include "../../support/latency_samples.h"
#include "esp_timer.h"
#include "kernel/ports/provider_mailbox.h"
#include "motor_contract.h"
#include "motor_execution.h"
#include <ayni/hosts/sim.h>
#include <functional>
#include <gtest/gtest.h>
namespace {
using namespace wifichat;
MotorCommand output_command{};
unsigned replacements = 0;
bool fail_replacement = false;
std::function<void()> during_stop;
ayni_provider_completion_t *test_completion = nullptr;
ayni_attempt_ref_v1 test_attempt{};
} // namespace
namespace wifichat {
esp_err_t MotorOutput::initialize() {
    state_ = State::idle;
    return ESP_OK;
}
esp_err_t MotorOutput::start(const MotorCommand &command) {
    if (!valid_motor_waveform(command)) {
        return ESP_FAIL;
    }
    output_command = command;
    state_ = State::running;
    return ESP_OK;
}
esp_err_t MotorOutput::replace_active(const MotorCommand &command) {
    if (!valid_motor_waveform(command)) {
        return ESP_FAIL;
    }
    ++replacements;
    if (fail_replacement) {
        state_ = State::fault;
        return ESP_FAIL;
    }
    output_command = command;
    return ESP_OK;
}
esp_err_t MotorOutput::transition(const MotorCommand &command) {
    return start(command);
}
MotorOutput::Poll MotorOutput::poll_segment() {
    return Poll::running;
}
esp_err_t MotorOutput::stop() {
    active_percent_ = {};
    if (state_ != State::fault) {
        state_ = State::idle;
    }
    if (during_stop) {
        auto callback = std::move(during_stop);
        during_stop = {};
        callback();
    }
    return ESP_OK;
}
bool MotorOutput::disabled() const {
    return state_ != State::running;
}
bool MotorOutput::counts(std::array<int, 4> &out) const {
    out = {};
    return true;
}
esp_err_t MotorOutput::close() {
    state_ = State::closed;
    return ESP_OK;
}
struct MotorExecutionTestPeer {
    static void setup(MotorExecution &self) {
        ASSERT_EQ(self.initialize_owner(), AYNI_OK);
        self.mode_ = ControlMode::armed;
        self.accepting_ = true;
        self.heartbeat_capture_ = uint64_t(esp_timer_get_time());
    }
    static void start(MotorExecution &self, bool streaming = true, uint32_t limit = 90) {
        self.limit_ = limit;
        const auto ports = self.ports();
        ayni_permit_epochs_v1 epochs{1, 1, ports.executor_boot_epoch, 1, 1, 0};
        ayni_executor_nonce_v1 nonce{};
        ASSERT_EQ(self.challenge(epochs, nonce), AYNI_OK);
        MotorExecution::Pending incoming;
        incoming.completion = test_completion;
        incoming.attempt = test_attempt;
        incoming.call = 10;
        incoming.streaming = streaming;
        incoming.session_goal = 10;
        incoming.target_seq = self.transport_.control_mailbox().reserve_start();
        incoming.trajectory.count = 1;
        incoming.trajectory.segments[0] = {20, 20, 1000};
        incoming.deadline_us = uint64_t(esp_timer_get_time()) + 1000000;
        auto &permit = incoming.permit;
        permit.struct_size = sizeof(permit);
        permit.form = AYNI_FORM_SESSION;
        permit.permit_id = 1;
        permit.control_domain_id = motor_domain;
        permit.epochs = epochs;
        permit.echoed_nonce = nonce.nonce;
        permit.operation_id = 10;
        permit.control_mode = 1;
        permit.lease_us = 1000000;
        permit.max_command_age_us = motor_sample_age_us;
        permit.envelope = motor_limits();
        self.begin(incoming);
        ASSERT_TRUE(self.pending_);
        ASSERT_EQ(self.advance_pending(), AYNI_OK);
        ASSERT_TRUE(self.pending_);
        ASSERT_EQ(self.pending_->phase, MotorExecution::Phase::running);
    }
    static void target(MotorExecution &self, int32_t percent, uint32_t duration = 1000) {
        ControlTarget target;
        target.source = ControlTargetSource::authorized_model;
        target.session_goal = 10;
        target.motor1 = target.motor2 = percent;
        target.duration_ms = duration;
        ASSERT_TRUE(
            self.transport_.control_mailbox().submit(target, uint64_t(esp_timer_get_time())));
        ASSERT_TRUE(self.transport_.control_mailbox().take_target(target));
        self.apply_target(target);
    }
    static auto sample(MotorExecution &self) { return self.sample(); }
    static auto mode(const MotorExecution &self) { return self.mode_; }
    static auto deadline(const MotorExecution &self) { return self.pending_->deadline_us; }
    static void hold(MotorExecution &self) { self.apply_hold(); }
    static void cycle(MotorExecution &self) { EXPECT_EQ(self.advance_owner(), AYNI_OK); }
    static void finish(MotorExecution &self) { self.end(AYNI_CALL_COMPLETED, AYNI_OK); }
    static void short_remaining(MotorExecution &self) {
        self.pending_->deadline_us = uint64_t(esp_timer_get_time()) + 20000;
    }
    static void cleanup(MotorExecution &self) {
        if (self.pending_) {
            self.stop_owner();
            ASSERT_EQ(self.post_result(), AYNI_OK);
        }
        EXPECT_TRUE(self.close_owner());
    }
};
} // namespace wifichat
namespace {
class MotorWorker : public testing::Test {
  protected:
    void SetUp() override {
        ayni::kernel::ProviderReservation reservation{};
        ASSERT_EQ(results.reserve({1, 1}, reservation), AYNI_OK);
        test_completion = results.completion(reservation);
        test_attempt = reservation.attempt;
        ASSERT_NE(test_completion, nullptr);
        replacements = 0;
        fail_replacement = false;
        during_stop = {};
        ASSERT_TRUE(transport.initialize());
        MotorExecutionTestPeer::setup(worker);
    }
    void TearDown() override {
        during_stop = {};
        MotorExecutionTestPeer::cleanup(worker);
    }
    ayni::hosts::sim::Host host;
    ayni_host_v1 ports = host.table();
    std::vector<uint64_t> result_storage = std::vector<uint64_t>(
        ayni::kernel::ProviderMailbox::storage_size({1, motor_reply_bytes_max}) / sizeof(uint64_t) +
        1);
    ayni::kernel::ProviderMailbox results{result_storage.data(), {1, motor_reply_bytes_max}, ports};
    ControlMailbox mailbox;
    Transport transport{mailbox};
    MotorExecution worker{transport};
};
TEST_F(MotorWorker, NewHardStopDuringSoftStopIsNeverConfirmedAsArmed) {
    MotorExecutionTestPeer::start(worker);
    transport.request_motor_soft_stop();
    during_stop = [&] { transport.request_motor_stop(); };
    MotorExecutionTestPeer::cycle(worker);
    MotorExecutionTestPeer::cycle(worker);
    EXPECT_EQ(MotorExecutionTestPeer::mode(worker), ControlMode::latched);
    EXPECT_FALSE(mailbox.stop_pending());
    EXPECT_EQ(mailbox.snapshot().control_mode, uint8_t(ControlMode::latched));
    EXPECT_TRUE(mailbox.target_blocked(mailbox.reserve_start()));
}
TEST_F(MotorWorker, NaturalCompletionDoesNotConsumeConcurrentHardStop) {
    MotorExecutionTestPeer::start(worker);
    during_stop = [&] { transport.request_motor_stop(); };
    MotorExecutionTestPeer::finish(worker);
    EXPECT_TRUE(mailbox.stop_pending());
    MotorExecutionTestPeer::cycle(worker);
    EXPECT_EQ(MotorExecutionTestPeer::mode(worker), ControlMode::latched);
    EXPECT_FALSE(mailbox.stop_pending());
}
TEST_F(MotorWorker, HotTargetObeysOperatorLimitBeforeMutatingDesiredOutput) {
    MotorExecutionTestPeer::start(worker, true, 30);
    MotorExecutionTestPeer::target(worker, 80);
    EXPECT_EQ(mailbox.snapshot().reject_reason, ControlRejectReason::out_of_envelope);
    ASSERT_EQ(MotorExecutionTestPeer::sample(worker), AYNI_OK);
    EXPECT_EQ(replacements, 0U);
    EXPECT_EQ(output_command.first_percent, 20);
}
TEST_F(MotorWorker, HotSameDirectionReverseHoldAndStickyStopUseOneSession) {
    MotorExecutionTestPeer::start(worker);
    const auto initial = mailbox.snapshot().applied_seq;
    MotorExecutionTestPeer::target(worker, 40);
    ASSERT_EQ(MotorExecutionTestPeer::sample(worker), AYNI_OK);
    EXPECT_EQ(output_command.first_percent, 40);
    EXPECT_GT(mailbox.snapshot().applied_seq, initial);
    MotorExecutionTestPeer::target(worker, -40);
    ASSERT_EQ(MotorExecutionTestPeer::sample(worker), AYNI_OK);
    EXPECT_EQ(output_command.first_percent, -40);
    ASSERT_TRUE(mailbox.request_hold(25));
    MotorExecutionTestPeer::hold(worker);
    ASSERT_EQ(MotorExecutionTestPeer::sample(worker), AYNI_OK);
    EXPECT_EQ(output_command.first_percent, -10);
    transport.request_motor_soft_stop();
    MotorExecutionTestPeer::cycle(worker);
    EXPECT_FALSE(mailbox.stop_pending());
    EXPECT_EQ(MotorExecutionTestPeer::mode(worker), ControlMode::armed);
}
TEST_F(MotorWorker, HoldOnDialogueRunStopsBeforeAnotherHardwareReplacement) {
    MotorExecutionTestPeer::start(worker, false);
    ASSERT_TRUE(mailbox.request_hold(25));
    MotorExecutionTestPeer::cycle(worker);
    EXPECT_EQ(replacements, 0U);
    EXPECT_FALSE(mailbox.stop_pending());
    EXPECT_EQ(mailbox.snapshot().reject_reason, ControlRejectReason::no_active_session);
}
TEST_F(MotorWorker, ReplacementFailureLatchesAndPreservesInternalCause) {
    MotorExecutionTestPeer::start(worker);
    MotorExecutionTestPeer::target(worker, 40);
    fail_replacement = true;
    MotorExecutionTestPeer::cycle(worker);
    EXPECT_EQ(MotorExecutionTestPeer::mode(worker), ControlMode::latched);
    std::array<uint8_t, motor_reply_bytes_max> body{};
    uint32_t required = 0;
    ayni::kernel::ProviderMessage message{};
    ayni::kernel::ProviderReceipt receipt{};
    ASSERT_EQ(results.read_next(body.data(), uint32_t(body.size()), required, message, receipt),
              AYNI_OK);
    EXPECT_EQ(message.kind, ayni::kernel::ProviderMessageKind::failed);
    EXPECT_EQ(message.status, AYNI_INTERNAL);
}
TEST_F(MotorWorker, FinalShortHardwareBlockDoesNotBecomeExpiredCancellation) {
    MotorExecutionTestPeer::start(worker);
    MotorExecutionTestPeer::target(worker, 40);
    MotorExecutionTestPeer::short_remaining(worker);
    EXPECT_EQ(MotorExecutionTestPeer::sample(worker), AYNI_OK);
    EXPECT_GE(output_command.duration_ms, motor_duration_step_ms);
    EXPECT_LT(output_command.duration_ms, motor_duration_ms_min);
}
TEST_F(MotorWorker, HotTargetMayShortenButNeverRestoreAnEarlierDeadline) {
    MotorExecutionTestPeer::start(worker);
    MotorExecutionTestPeer::target(worker, 30, 500);
    const auto shortened = MotorExecutionTestPeer::deadline(worker);
    MotorExecutionTestPeer::target(worker, 40, 1000);
    EXPECT_EQ(MotorExecutionTestPeer::deadline(worker), shortened);
}
} // namespace

TEST_F(MotorWorker, SoftwareHotSameDirectionReverseAndHoldHaveBoundedMeasuredLatency) {
    // ADR-0114: CPU wall-clock for real executor/Gate/loop. Fake SDK output is not a board
    // waveform/deadtime or complete model-decision-to-output latency measurement.
    MotorExecutionTestPeer::start(worker);
    ayni::test::LatencySamples same, reverse, hold;
    for (unsigned at = 0; at < 256; ++at) {
        auto began = ayni::test::LatencySamples::Clock::now();
        MotorExecutionTestPeer::target(worker, 40);
        ASSERT_EQ(MotorExecutionTestPeer::sample(worker), AYNI_OK);
        same.add(began);
        began = ayni::test::LatencySamples::Clock::now();
        MotorExecutionTestPeer::target(worker, -40);
        ASSERT_EQ(MotorExecutionTestPeer::sample(worker), AYNI_OK);
        reverse.add(began);
        began = ayni::test::LatencySamples::Clock::now();
        ASSERT_TRUE(mailbox.request_hold(25));
        MotorExecutionTestPeer::hold(worker);
        ASSERT_EQ(MotorExecutionTestPeer::sample(worker), AYNI_OK);
        hold.add(began);
    }
    same.print("executor_same_direction_sdk_stub");
    reverse.print("executor_reverse_sdk_stub");
    hold.print("executor_hold_sdk_stub");
    EXPECT_LT(same.percentile(99), 1000);
    EXPECT_LT(reverse.percentile(99), 1000);
    EXPECT_LT(hold.percentile(99), 1000);
}

TEST(MotorWaveform, InternalShortBlocksKeepPublicInputLimitsAndPwmChecks) {
    // ADR-0114: the production MotorOutput validator accepts final internal RMT blocks only.
    for (const uint32_t duration : {5U, 20U, 45U}) {
        EXPECT_TRUE(wifichat::valid_motor_waveform({20, -20, duration}));
        EXPECT_FALSE(wifichat::valid_motor_command({20, -20, duration}));
    }
    for (const uint32_t duration : {0U, 4U, 49U, 60005U}) {
        EXPECT_FALSE(wifichat::valid_motor_waveform({20, -20, duration}));
    }
    EXPECT_FALSE(wifichat::valid_motor_waveform({91, 0, 20}));
    EXPECT_TRUE(wifichat::valid_motor_command({20, -20, 50}));
}

TEST_F(MotorWorker, StructuredTargetCannotStartOutputWithoutAnAuthorizedStream) {
    // ADR-0114: local structured controls are setpoints for existing authorization only.
    wifichat::ControlTarget target;
    target.motor1 = target.motor2 = 30;
    target.duration_ms = 500;
    ASSERT_TRUE(transport.submit_motor_target(target));
    MotorExecutionTestPeer::cycle(worker);
    EXPECT_EQ(mailbox.snapshot().reject_reason, ControlRejectReason::no_active_session);
    EXPECT_EQ(replacements, 0U);
    EXPECT_EQ(mailbox.snapshot().applied_seq, 0U);
}
