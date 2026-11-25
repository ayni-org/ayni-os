#include "motor_execution.h"
#include "motor_contract.h"
#include "wake.h"
#include <algorithm>
#include <ayni/values.hpp>
#include <cstdio>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <new>
namespace wifichat {
namespace {
constexpr uint64_t heartbeat_us = 3000000;
uint64_t now() {
    return uint64_t(esp_timer_get_time());
}
bool same(const ayni_permit_epochs_v1 &a, const ayni_permit_epochs_v1 &b) {
    return a.kernel_boot_epoch == b.kernel_boot_epoch && a.authority_epoch == b.authority_epoch &&
           a.executor_boot_epoch == b.executor_boot_epoch && a.safety_epoch == b.safety_epoch &&
           a.calibration_version == b.calibration_version && a.reserved == b.reserved;
}
} // namespace
ayni_status_t MotorExecution::initialize_owner() {
    // ports() already fixed this identity before Core registration.
    auto *memory = heap_caps_malloc(sizeof(MotorOutput), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!memory) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    output_ = new (memory) MotorOutput;
    if (output_->initialize() != ESP_OK) {
        return AYNI_INTERNAL;
    }
    size_t alignment = 0;
    auto rc = ayni_control_memory_requirements(&gate_bytes_, &alignment);
    if (rc != AYNI_OK) {
        return rc;
    }
    gate_memory_ =
        heap_caps_aligned_alloc(alignment, gate_bytes_, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!gate_memory_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    size_t session_align = 0;
    rc = ayni_control_session_memory_requirements(&session_bytes_, &session_align);
    if (rc != AYNI_OK) {
        return rc;
    }
    session_memory_ = heap_caps_aligned_alloc(session_align, session_bytes_,
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!session_memory_) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return ayni_control_session_create(session_memory_, session_bytes_, &session_);
}
ayni_executor_v1 MotorExecution::ports() {
    if (!executor_boot_) {
        executor_boot_ = (uint64_t(esp_random()) << 32) | esp_random();
        if (!executor_boot_) {
            executor_boot_ = 1;
        }
    }
    return {sizeof(ayni_executor_v1),
            motor_version,
            motor_text("motor.executor"),
            motor_domain,
            executor_boot_,
            1,
            1,
            1,
            motor_procedure_us,
            motor_session_lease_us,
            motor_sample_age_us,
            motor_poll_us,
            this,
            challenge_port,
            renew_port,
            stop_port,
            status_port,
            AYNI_EXECUTOR_FLAG_INLINE_STOP_PROOF,
            0};
}
void MotorExecution::mark_output_start(uint64_t call, uint64_t at_us) {
    portENTER_CRITICAL(&output_mutex_);
    auto &slot = output_intervals_[output_count_++ % output_intervals_.size()];
    last_overwritten_stop_us_ = slot.stop_us;
    slot = {call, at_us, 0};
    portEXIT_CRITICAL(&output_mutex_);
}
void MotorExecution::mark_output_stop(uint64_t call, uint64_t at_us) {
    portENTER_CRITICAL(&output_mutex_);
    if (output_count_) {
        auto &slot = output_intervals_[(output_count_ - 1U) % output_intervals_.size()];
        if (slot.call == call && !slot.stop_us) {
            slot.stop_us = at_us;
        }
    }
    portEXIT_CRITICAL(&output_mutex_);
}
bool MotorExecution::output_intervals(uint64_t from_us, uint64_t to_us, OutputIntervals &out) {
    portENTER_CRITICAL(&output_mutex_);
    const auto retained = std::min<uint64_t>(output_count_, output_intervals_.size());
    size_t count = 0;
    bool complete = last_overwritten_stop_us_ <= from_us;
    for (auto i = output_count_ - retained; i < output_count_; ++i) {
        const auto &span = output_intervals_[i % output_intervals_.size()];
        if (span.start_us < to_us && (!span.stop_us || span.stop_us > from_us)) {
            out[count++] = span;
            complete = complete && span.stop_us != 0;
        }
    }
    portEXIT_CRITICAL(&output_mutex_);
    return complete;
}
void MotorExecution::trace_handoff(const char *phase, uint64_t call, uint64_t at_us) {
    std::array<char, 128> line{};
    const auto written = std::snprintf(
        line.data(), line.size(), "wifi motor_call call=%llu phase=%s at_us=%llu\n",
        static_cast<unsigned long long>(call), phase, static_cast<unsigned long long>(at_us));
    if (written > 0 && size_t(written) < line.size()) {
        transport_.diagnostics().capture({line.data(), size_t(written)});
    }
}
void MotorExecution::trace(const char *stage, ayni_status_t status) {
    ayni_control_info_v1 info{};
    info.struct_size = sizeof(info);
    const auto read = gate_ ? ayni_control_get_info(gate_, &info) : AYNI_NOT_READY;
    std::array<int, 4> edges{};
    const bool counted = output_ && output_->counts(edges);
    char message[384];
    const auto count = std::snprintf(
        message, sizeof(message),
        "motor stage=%s status=%ld mode=%u permit=%llu gate=%lu cause=%lu sample=%llu info=%ld "
        "sdk=%ld edges38/39/40/41=%d/%d/%d/%d counted=%u gap_max_us=%llu sample_age_us=%lld",
        stage, (long)status, unsigned(mode_), (unsigned long long)current_permit_,
        (unsigned long)info.phase, (unsigned long)info.stop_cause,
        (unsigned long long)info.last_sample_sequence, (long)read,
        output_ ? (long)output_->last_error() : -1L, edges[0], edges[1], edges[2], edges[3],
        unsigned(counted), (unsigned long long)cycle_gap_max_us_,
        pending_ && pending_->last_sample_us ? (long long)(now() - pending_->last_sample_us)
                                             : -1LL);
    if (count > 0 && size_t(count) < sizeof(message)) {
        transport_.diagnostics().capture({message, size_t(count)});
        transport_.send_latest_status(current_permit_, message, size_t(count));
    }
}
ayni_status_t MotorExecution::queue(const PortRequest &request) {
    if (!request.completion || request.kind < AYNI_EXECUTOR_CHALLENGE ||
        request.kind > AYNI_EXECUTOR_STATUS) {
        return AYNI_INVALID_ARGUMENT;
    }
    if ((inbox_access_.load(std::memory_order_acquire) & inbox_closed) != 0U) {
        return AYNI_DENIED;
    }
    Message message{};
    message.kind = MessageKind::port;
    message.port = request;
    return send(message) ? AYNI_OK : AYNI_BUSY;
}
ayni_status_t MotorExecution::challenge_port(void *ctx, ayni_attempt_ref_v1 id,
                                             const ayni_permit_epochs_v1 *epochs,
                                             ayni_executor_completion_t *completion) {
    if (!epochs) {
        return AYNI_INVALID_ARGUMENT;
    }
    return static_cast<MotorExecution *>(ctx)->queue(
        {AYNI_EXECUTOR_CHALLENGE, id, completion, *epochs, 0, 0});
}
ayni_status_t MotorExecution::renew_port(void *ctx, ayni_attempt_ref_v1 id,
                                         const ayni_permit_renewal_v1 *renewal,
                                         ayni_executor_completion_t *completion) {
    if (!renewal) {
        return AYNI_INVALID_ARGUMENT;
    }
    return static_cast<MotorExecution *>(ctx)->queue({AYNI_EXECUTOR_RENEW, id, completion,
                                                      renewal->epochs, renewal->permit_id,
                                                      renewal->echoed_nonce});
}
ayni_status_t MotorExecution::stop_port(void *ctx, ayni_attempt_ref_v1 id, uint64_t permit,
                                        ayni_executor_completion_t *completion) {
    return static_cast<MotorExecution *>(ctx)->queue(
        {AYNI_EXECUTOR_STOP, id, completion, {}, permit, 0});
}
ayni_status_t MotorExecution::status_port(void *ctx, ayni_attempt_ref_v1 id, uint64_t permit,
                                          ayni_executor_completion_t *completion) {
    return static_cast<MotorExecution *>(ctx)->queue(
        {AYNI_EXECUTOR_STATUS, id, completion, {}, permit, 0});
}
ayni_control_observation_v1 MotorExecution::live() const {
    auto epochs = epochs_;
    if (mode_ == Mode::latched) {
        ++epochs.safety_epoch;
    }
    const auto heartbeat = heartbeat_capture_.load(std::memory_order_acquire);
    return {epochs,
            uint32_t(accepting_.load(std::memory_order_acquire) && mode_ == Mode::armed &&
                     !transport_.control_mailbox().hard_stop_requested() && heartbeat &&
                     now() - heartbeat < heartbeat_us),
            0};
}
ayni_status_t MotorExecution::challenge(const ayni_permit_epochs_v1 &expected,
                                        ayni_executor_nonce_v1 &nonce) {
    // Allow challenges while an AYNI_FORM_SESSION is active so Core can renew its short lease
    // without interrupting hardware output (ADR-0112).
    if (!live().interlocks_ok || (pending_ && pending_->phase == Phase::result) ||
        expected.executor_boot_epoch != executor_boot_ || expected.safety_epoch != 1 ||
        expected.calibration_version != 1) {
        return AYNI_DENIED;
    }
    if (!gate_) {
        const ayni_control_config_v1 config{sizeof(config),      AYNI_ABI_MAJOR,
                                            AYNI_ABI_MINOR,      1,
                                            motor_domain,        expected,
                                            motor_procedure_us,  motor_session_lease_us,
                                            motor_sample_age_us, 0,
                                            motor_limits()};
        auto rc = ayni_control_create(&config, now(), gate_memory_, gate_bytes_, &gate_);
        if (rc != AYNI_OK) {
            return rc;
        }
        epochs_ = expected; // Trusted Core boot is bound once for this executor lifetime.
    } else if (!same(epochs_, expected)) {
        return AYNI_STALE;
    }
    const auto issued = ayni_control_nonce(gate_, now(), &nonce);
    if (issued == AYNI_OK) {
        admission_.issued(nonce.nonce);
    }
    return issued;
}
ayni_status_t MotorExecution::status(uint64_t permit, ayni_executor_status_v1 &out) {
    if (!gate_ || permit > current_permit_) {
        refused_through_ = std::max(refused_through_, permit);
        return AYNI_NOT_FOUND; // Future submit of this exact ID is permanently refused.
    }
    if (permit != current_permit_) {
        return AYNI_STALE;
    }
    const auto observation = live();
    const auto checked = ayni_control_check(gate_, now(), &observation);
    if (checked == AYNI_INVALID_ARGUMENT) {
        return checked;
    }
    out = {};
    out.struct_size = sizeof(out);
    out.target = AYNI_TARGET_UNVERIFIED;
    out.control.struct_size = sizeof(out.control);
    // NOT_READY before the first permitted output sample still has an ACTIVE gate.
    return ayni_control_get_info(gate_, &out.control);
}
ayni_status_t MotorExecution::serve(PortRequest &request) {
    ayni_executor_response_v1 response{};
    response.struct_size = sizeof(response);
    response.kind = request.kind;
    response.request = request.identity;
    if (request.kind == AYNI_EXECUTOR_CHALLENGE) {
        response.status = challenge(request.epochs, response.output.nonce);
    } else if (request.kind == AYNI_EXECUTOR_RENEW) {
        if (!session_ || !pending_ || !current_permit_ || request.permit_id != current_permit_) {
            response.status = AYNI_NOT_FOUND;
        } else if (!admission_.permits(request.echoed_nonce)) {
            response.status = AYNI_DENIED;
        } else {
            const ayni_permit_renewal_v1 renewal{sizeof(renewal), 0, request.permit_id,
                                                 request.echoed_nonce, request.epochs};
            const auto observation = live();
            response.status = ayni_control_session_renew(session_, &renewal, now(), &observation);
        }
    } else if (request.kind == AYNI_EXECUTOR_STOP) {
        if (request.permit_id == current_permit_ && current_permit_) {
            if (pending_ && pending_->phase != Phase::result) {
                end(AYNI_CALL_CANCELLED, AYNI_DENIED);
                response.status =
                    (pending_ && pending_->phase == Phase::result) ? AYNI_OK : AYNI_INTERNAL;
            } else {
                response.status = stop_output();
                if (response.status != AYNI_OK) {
                    mode_ = Mode::latched;
                }
            }
        } else {
            response.status = AYNI_NOT_FOUND;
        }
    } else {
        response.status = status(request.permit_id, response.output.status);
    }
    const auto posted = ayni_executor_post(request.completion, &response);
    if (response.status != AYNI_OK) {
        trace(request.kind == AYNI_EXECUTOR_CHALLENGE ? "challenge" : "executor-port",
              response.status);
    }
    // Core may have abandoned this exact answer after its bounded wait. Never replay it.
    return posted == AYNI_INVALID_STATE ? AYNI_OK : posted;
}
void MotorExecution::reject(const Pending &request, ayni_status_t cause) {
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = request.attempt;
    result.outcome = AYNI_CALL_FAILED;
    result.status = cause;
    result.effect = AYNI_CALL_EFFECT_NONE;
    const auto posted = ayni_provider_post(request.completion, &result);
    if (posted != AYNI_OK && posted != AYNI_INVALID_STATE) {
        worker_error_.store(posted, std::memory_order_release);
        wake_owner();
    }
    trace("start-refused", cause);
}
void MotorExecution::begin(const Pending &incoming) {
    // UI state-notice backpressure (terminal_notice_) must not deny an authorized motor start
    // (ADR-0111); Mode::armed/gate/interlocks/output_->disabled() remain the safety gates.
    bool within_limit =
        incoming.streaming
            ? incoming.trajectory.count == 1 && valid_motor_command(incoming.trajectory.segments[0])
            : valid_motor_trajectory(incoming.trajectory);
    for (uint32_t at = 0; within_limit && at < incoming.trajectory.count; ++at) {
        const auto &seg = incoming.trajectory.segments[at];
        within_limit = within_operator_limit(seg);
    }
    if (pending_ || transport_.control_mailbox().target_blocked(incoming.target_seq) || !gate_ ||
        !session_ || !live().interlocks_ok || incoming.permit.permit_id <= refused_through_ ||
        !admission_.permits(incoming.permit.echoed_nonce) || !output_->disabled() ||
        !within_limit || (incoming.streaming && now() >= incoming.deadline_us)) {
        reject(incoming, AYNI_DENIED);
        return;
    }
    const auto limits = motor_limits();
    for (uint32_t at = 0; at < incoming.trajectory.count; ++at) {
        const auto &cmd = incoming.trajectory.segments[at];
        const int64_t vals[]{cmd.first_percent, cmd.second_percent, cmd.duration_ms};
        auto &segment = session_segments_[at];
        segment.struct_size = sizeof(segment);
        segment.field_count = limits.count;
        // Hardware RMT loop counts time each segment; ControlSession advances via advance_segment.
        segment.duration_us = 0;
        for (uint32_t f = 0; f < limits.count; ++f) {
            segment.fields[f] = {limits.fields[f].field_id, limits.fields[f].unit, vals[f]};
        }
    }
    const auto observation = live();
    const auto started =
        ayni_control_session_open(session_, gate_, &incoming.permit, session_segments_.data(),
                                  incoming.trajectory.count, now(), &observation);
    if (started != AYNI_OK) {
        reject(incoming, started);
        return;
    }
    pending_ = incoming;
    pending_->accepted_at_us = now();
    const auto held = transport_.control_mailbox().hold();
    last_hold_seq_ = held.seq;
    hold_scale_ =
        held.seq > incoming.target_seq ? held.scale_percent : AYNI_CONTROL_LOOP_FULL_SCALE;
    transport_.control_mailbox().mark_start_received(incoming.target_seq, now());
    loop_ = {};
    loop_.struct_size = sizeof(loop_);
    loop_.field_count = 2;
    constexpr uint64_t max_cycle_gap_us = 20000;
    loop_.max_cycle_gap_us = max_cycle_gap_us;
    loop_.fields[0] = limits.fields[0].field_id;
    loop_.fields[1] = limits.fields[1].field_id;
    current_permit_ = incoming.permit.permit_id;
    sequence_ = 0;
    trace("permit-accepted", AYNI_OK);
}
// RMT's finite waveform also obeys the original Goal deadline, including initial handoff delay.
ayni_status_t MotorExecution::hardware_command(const MotorCommand &desired,
                                               MotorCommand &out) const {
    out = desired;
    if (pending_->streaming) {
        const auto at = now();
        const auto remaining_ms =
            pending_->deadline_us > at ? (pending_->deadline_us - at) / 1000U : 0;
        out.duration_ms = uint32_t(remaining_ms / motor_duration_step_ms) * motor_duration_step_ms;
        if (out.duration_ms < motor_duration_step_ms) {
            return AYNI_EXPIRED;
        }
    }
    return AYNI_OK;
}
// Shape the immutable desired target, validate THAT effective output through PermitGate,
// then update hardware only if duty changed. Desired/queued trajectory remain untouched.
ayni_status_t MotorExecution::sample() {
    auto &p = *pending_;
    const auto observation = live();
    const auto sampled_at = now();
    auto desired = session_segments_[p.segment_index];
    ayni_control_segment_v1 effective{};
    ayni_control_loop_action_t action = AYNI_LOOP_SOFT_STOP;
    // No physical distance sensor is enabled in this bench composition. Required-source
    // freshness/quality and cycle limits are handled by the same portable limiter when configured.
    auto rc = ayni_control_loop_limit(&loop_, &desired, nullptr, 0, sampled_at, hold_scale_,
                                      &effective, &action, &loop_);
    if (rc != AYNI_OK || action != AYNI_LOOP_APPLY) {
        transport_.control_mailbox().mark_rejected(p.target_seq,
                                                   ControlRejectReason::interlock_fault,
                                                   rc == AYNI_OK ? AYNI_EXPIRED : rc, sampled_at);
        return rc == AYNI_OK ? AYNI_EXPIRED : rc;
    }
    ayni_control_session_step_v1 step{};
    step.struct_size = sizeof(step);
    rc = ayni_control_session_tick_effective(session_, sampled_at, &observation, &effective, &step);
    if (rc != AYNI_OK || step.state != AYNI_CONTROL_SESSION_ACTIVE) {
        const auto rejected =
            hold_scale_ < AYNI_CONTROL_LOOP_FULL_SCALE ? uint64_t(last_hold_seq_) : p.target_seq;
        transport_.control_mailbox().mark_rejected(rejected,
                                                   rc == AYNI_DENIED
                                                       ? ControlRejectReason::out_of_envelope
                                                       : ControlRejectReason::interlock_fault,
                                                   rc == AYNI_OK ? AYNI_EXPIRED : rc, sampled_at);
        return rc == AYNI_OK ? AYNI_EXPIRED : rc;
    }
    p.gate_ok_us = now();
    const MotorCommand shaped{int32_t(step.active.fields[0].value),
                              int32_t(step.active.fields[1].value),
                              p.trajectory.segments[p.segment_index].duration_ms};
    if (p.phase == Phase::running && (p.effective.first_percent != shaped.first_percent ||
                                      p.effective.second_percent != shaped.second_percent ||
                                      (p.streaming && p.waveform_deadline_us != p.deadline_us))) {
        auto hardware = shaped;
        const auto bounded = hardware_command(shaped, hardware);
        if (bounded != AYNI_OK) {
            return bounded;
        }
        if (output_->replace_active(hardware) != ESP_OK) {
            return AYNI_INTERNAL;
        }
        p.waveform_deadline_us = p.deadline_us;
        const auto timing = output_->replacement_timing();
        transport_.diagnostics().record_deferred(
            "wifi motor_hot call=%llu phase=%s gap_us=%llu\n", (unsigned long long)p.call,
            timing.reverse_or_coast ? "reverse_gap_us" : "hot_gap_us",
            (unsigned long long)timing.gap_us);
    }
    p.effective = shaped;
    p.last_sample_us = sampled_at;
    ++sequence_;
    if (p.phase == Phase::running && p.target_seq) {
        report_application(now());
    }
    return AYNI_OK;
}
// Capture first actual application once per accepted target/hold, never printing on control worker.
void MotorExecution::report_application(uint64_t applied_at_us) {
    auto &p = *pending_;
    const auto applied_seq = std::max(p.target_seq, uint64_t(last_hold_seq_));
    transport_.control_mailbox().mark_applied(applied_seq, p.effective.first_percent,
                                              p.effective.second_percent, applied_at_us);
    if (p.target_seq != p.reported_target_seq) {
        transport_.diagnostics().record_deferred(
            "wifi control_apply seq=%llu call=%llu accepted_at_us=%llu gate_ok_us=%llu "
            "applied_at_us=%llu\n",
            (unsigned long long)p.target_seq, (unsigned long long)p.call,
            (unsigned long long)p.accepted_at_us, (unsigned long long)p.gate_ok_us,
            (unsigned long long)applied_at_us);
        p.reported_target_seq = p.target_seq;
    }
    if (last_hold_seq_ > p.target_seq && last_hold_seq_ != p.reported_hold_seq) {
        transport_.diagnostics().record_deferred(
            "wifi control_hold seq=%lu phase=hold_applied applied_at_us=%llu scale_pct=%lu\n",
            (unsigned long)last_hold_seq_, (unsigned long long)applied_at_us,
            (unsigned long)hold_scale_);
        p.reported_hold_seq = last_hold_seq_;
    }
}
// Latest-wins may only narrow/update an already active independent session. A model target
// additionally names its exact session; structured local controls select the sole active one.
void MotorExecution::apply_target(const ControlTarget &target) {
    auto &mailbox = transport_.control_mailbox();
    ControlRejectReason reason = ControlRejectReason::none;
    if (mailbox.target_blocked(target.seq)) {
        reason = ControlRejectReason::stop_pending;
    } else if (mode_ != Mode::armed) {
        reason = ControlRejectReason::disarmed_or_latched;
    } else if (!pending_ || !pending_->streaming || pending_->phase == Phase::result ||
               (target.session_goal && target.session_goal != pending_->session_goal)) {
        reason = ControlRejectReason::no_active_session;
    } else if (!live().interlocks_ok) {
        reason = ControlRejectReason::interlock_fault;
    }
    if (reason != ControlRejectReason::none) {
        mailbox.mark_rejected(target.seq, reason, AYNI_DENIED, now());
        return;
    }
    auto &p = *pending_;
    const MotorCommand command{target.motor1, target.motor2, target.duration_ms};
    if (!valid_motor_command(command) || !within_operator_limit(command) ||
        target.duration_ms > p.trajectory.segments[0].duration_ms) {
        mailbox.mark_rejected(target.seq, ControlRejectReason::out_of_envelope, AYNI_DENIED, now());
        return;
    }
    auto desired = session_segments_[0];
    desired.fields[0].value = target.motor1;
    desired.fields[1].value = target.motor2;
    const auto updated = ayni_control_session_replace_target(session_, &desired, now());
    if (updated != AYNI_OK) {
        mailbox.mark_rejected(target.seq, ControlRejectReason::out_of_envelope, updated, now());
        return;
    }
    session_segments_[0] = desired;
    p.target_seq = target.seq;
    p.accepted_at_us = target.accepted_at_us;
    p.deadline_us = std::min(p.deadline_us, now() + uint64_t(target.duration_ms) * 1000U);
    if (target.seq > last_hold_seq_) {
        hold_scale_ = AYNI_CONTROL_LOOP_FULL_SCALE;
    }
}
bool MotorExecution::within_operator_limit(const MotorCommand &command) const {
    return command.first_percent >= -int32_t(limit_) && command.first_percent <= int32_t(limit_) &&
           command.second_percent >= -int32_t(limit_) && command.second_percent <= int32_t(limit_);
}
void MotorExecution::apply_hold() {
    const auto held = transport_.control_mailbox().hold();
    if (held.seq == last_hold_seq_) {
        return;
    }
    last_hold_seq_ = held.seq;
    hold_scale_ = held.scale_percent;
    if (hold_scale_ < AYNI_CONTROL_LOOP_FULL_SCALE && !pending_) {
        transport_.control_mailbox().mark_rejected(held.seq, ControlRejectReason::no_active_session,
                                                   AYNI_NOT_READY, now());
    }
    if (hold_scale_ < AYNI_CONTROL_LOOP_FULL_SCALE && pending_ && !pending_->streaming) {
        transport_.control_mailbox().mark_rejected(held.seq, ControlRejectReason::no_active_session,
                                                   AYNI_DENIED, now());
        // A dialogue-owned trajectory cannot survive cancellation, even if its union happens
        // to contain low speeds. Only an independently authorized stream supports hold.
        transport_.request_motor_soft_stop();
    }
}
ayni_status_t MotorExecution::stop_output(std::optional<ControlStopRequest> requested) {
    if (requested && requested->kind == ControlStopKind::hard_estop) {
        mode_ = Mode::latched;
    }
    admission_.disable(); // Any start carrying a pre-stop handshake is permanently refused.
    ayni_status_t stopped = AYNI_OK;
    if (session_ && gate_ && current_permit_) {
        stopped = ayni_control_session_soft_stop(session_, now());
    } else if (gate_ && current_permit_) {
        stopped = ayni_control_stop(gate_, current_permit_, now());
    }
    if (!output_ || output_->stop() != ESP_OK || !output_->disabled()) {
        return AYNI_INTERNAL;
    }
    if (stopped != AYNI_OK) {
        return stopped;
    }
    const auto readback_at_us = now();
    // C-STOP-006: actual controller output readback, separate from wheel/target facts.
    // Only a successful stop() plus gate confirmation may advance the mailbox stop frontier.
    if (gate_ && current_permit_) {
        stopped = session_ ? ayni_control_session_confirm_stopped(session_, now())
                           : ayni_control_confirm_stopped(gate_, current_permit_, now());
    }
    if (stopped == AYNI_OK && requested) {
        ayni_control_info_v1 info{};
        info.struct_size = sizeof(info);
        if (!current_permit_ || (ayni_control_get_info(gate_, &info) == AYNI_OK &&
                                 info.phase == AYNI_CONTROL_STOPPED)) {
            const auto confirmed_at_us = now();
            if (transport_.control_mailbox().confirm_stop(*requested, mode_, info.stop_cause,
                                                          confirmed_at_us)) {
                transport_.diagnostics().record_deferred(
                    "wifi control_stop seq=%lu phase=stop_confirmed hw_low_readback_us=%llu "
                    "confirmed_at_us=%llu\n",
                    (unsigned long)requested->seq, (unsigned long long)readback_at_us,
                    (unsigned long long)confirmed_at_us);
            }
        }
    }
    return stopped;
}
void MotorExecution::end(ayni_call_outcome_t outcome, ayni_status_t status,
                         std::optional<ControlStopRequest> requested) {
    if (!pending_) {
        return;
    }
    auto &p = *pending_;
    p.outcome = outcome;
    p.status = status;
    trace("execution-end", status);
    if (stop_output(requested) != AYNI_OK) {
        mode_ = Mode::latched;
        p.outcome = AYNI_CALL_ABANDONED;
        p.status = AYNI_RESULT_UNKNOWN;
        return; // Keep the permit/resources; retry local disable, never invent STOPPED.
    }
    const auto stopped_us = now();
    mark_output_stop(p.call, stopped_us);
    // Exact local edge evidence can establish partial controller output after a known stop.
    // It does not measure wheel speed/position or repair an earlier UNKNOWN record.
    if (p.effect == Effect::possible) {
        std::array<int, 4> edges{};
        const auto &expected = output_->expected_counts();
        bool valid = output_->counts(edges), positive = false;
        for (size_t at = 0; at < edges.size(); ++at) {
            valid = valid && edges[at] >= 0 && uint32_t(edges[at]) <= expected[at];
            positive = positive || edges[at] > 0;
        }
        if (valid && positive) {
            p.effect = Effect::partial;
        }
    }
    trace_handoff("output_stopped", p.call, stopped_us);
    p.phase = Phase::result; // Local output-stop fact ends a timed PWM session; target stays
                             // unverified.
}
void MotorExecution::stop_owner(std::optional<ControlStopRequest> requested) {
    if (mode_ == Mode::closed) {
        return;
    }
    mode_ = Mode::latched;
    if (pending_ && pending_->phase != Phase::result) {
        end(AYNI_CALL_CANCELLED, AYNI_DENIED, requested);
    } else {
        stop_output(requested);
    }
}
ayni_status_t MotorExecution::soft_stop_owner(std::optional<ControlStopRequest> requested) {
    if (mode_ == Mode::closed) {
        return AYNI_OK;
    }
    if (pending_ && pending_->phase != Phase::result) {
        end(AYNI_CALL_CANCELLED, AYNI_DENIED, requested);
        if (pending_ && pending_->phase == Phase::result) {
            const auto posted = post_result();
            if (posted != AYNI_OK && posted != AYNI_BUSY) {
                return posted;
            }
            return AYNI_OK;
        }
        // end() failed stop_output() and already escalated mode_ to Mode::latched.
        return AYNI_INTERNAL;
    }
    const auto stopped = stop_output(requested);
    if (stopped != AYNI_OK) {
        mode_ = Mode::latched;
        publish_snapshot();
    }
    return stopped;
}
void MotorExecution::serve_soft_stop(const SoftStopRequest &request) {
    const auto requested = transport_.control_mailbox().pending_stop();
    if (requested && requested->kind == ControlStopKind::hard_estop) {
        mode_ = Mode::latched;
    }
    const auto stopped = soft_stop_owner(requested);
    const bool ok = stopped == AYNI_OK;
    char reply[192];
    const auto length = std::snprintf(
        reply, sizeof(reply),
        ok ? "{\"stop_requested\":true,\"stop_confirmed\":true,\"armed\":%s,"
             "\"output_stop\":\"gpio-matrix-and-low-readback\"}"
           : "{\"stop_requested\":true,\"stop_confirmed\":false,\"armed\":false,\"latched\":true}",
        mode_ == Mode::armed ? "true" : "false");
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = request.attempt;
    result.outcome = ok ? AYNI_CALL_COMPLETED : AYNI_CALL_FAILED;
    result.status = ok ? AYNI_OK : AYNI_INTERNAL;
    result.effect = ok ? AYNI_CALL_EFFECT_COMPLETE : AYNI_CALL_EFFECT_UNKNOWN;
    if (length > 0 && size_t(length) < sizeof(reply)) {
        result.result = {reply, uint32_t(length)};
    }
    const auto posted = ayni_provider_post(request.completion, &result);
    if (posted != AYNI_OK && posted != AYNI_INVALID_STATE) {
        worker_error_.store(posted, std::memory_order_release);
        wake_owner();
    }
    trace("soft-stop", stopped);
}
ayni_status_t MotorExecution::post_result() {
    auto &p = *pending_;
    const auto &last =
        p.trajectory.segments[p.segment_index < p.trajectory.count ? p.segment_index : 0];
    char json[768];
    const auto length = std::snprintf(
        json, sizeof(json),
        "{\"permit_id\":%llu,\"segments_requested\":%lu,\"segments_completed\":%lu,"
        "\"total_duration_ms\":%lu,\"motor1_percent\":%ld,\"motor2_percent\":%ld,"
        "\"duration_ms\":%lu,\"duration_basis\":\"cumulative-pwm-ms\",\"output_started\":%s,"
        "\"output_waveform_complete\":%s,\"output_stop\":\"gpio-matrix-and-low-readback\","
        "\"operator_stopped\":null,\"operator_source\":\"not-requested\",\"local_status\":%ld,"
        "\"target\":\"unverified\",\"mode\":\"manual-bench-exception\","
        "\"independent_watchdog_verified\":false}",
        (unsigned long long)current_permit_, (unsigned long)p.trajectory.count,
        (unsigned long)p.segments_completed, (unsigned long)total_duration_ms(p.trajectory),
        (long)last.first_percent, (long)last.second_percent, (unsigned long)last.duration_ms,
        p.effect == Effect::none ? "false" : "true",
        !p.streaming && p.effect == Effect::complete ? "true" : "false", (long)p.status);
    if (length <= 0 || size_t(length) >= sizeof(json)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    ayni_provider_result_v1 result{};
    result.struct_size = sizeof(result);
    result.attempt = p.attempt;
    result.outcome = p.outcome;
    // C-CALL terminal evidence rules: cancellation is confirmed, uncertainty has no body.
    result.status = p.outcome == AYNI_CALL_CANCELLED ? AYNI_OK : p.status;
    result.effect = p.effect == Effect::none       ? AYNI_CALL_EFFECT_NONE
                    : p.effect == Effect::complete ? AYNI_CALL_EFFECT_COMPLETE
                    : p.effect == Effect::partial  ? AYNI_CALL_EFFECT_PARTIAL
                                                   : AYNI_CALL_EFFECT_UNKNOWN;
    ayni_executor_status_v1 exec_status{};
    if (p.outcome == AYNI_CALL_ABANDONED) {
        result.status = AYNI_RESULT_UNKNOWN;
        result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    } else {
        result.result = {json, uint32_t(length)};
        // Attach locally measured STOPPED proof from the colocated PermitGate (ADR-0110).
        if (current_permit_ && status(current_permit_, exec_status) == AYNI_OK &&
            exec_status.control.phase == AYNI_CONTROL_STOPPED) {
            result.execution_status = &exec_status;
        }
    }
    const auto posted = ayni_provider_post(p.completion, &result);
    if (posted == AYNI_OK || posted == AYNI_INVALID_STATE) {
        if (posted == AYNI_OK) {
            trace_handoff("result_posted", p.call, now());
        }
        pending_.reset();
        publish_snapshot();
        terminal_notice_ = current_permit_;
        publish_terminal();
        return AYNI_OK;
    }
    return posted;
}
void MotorExecution::publish_terminal() {
    if (!terminal_notice_ && !state_notice_) {
        return;
    }
    publish_snapshot();
    char state[512];
    const auto count = describe(state, sizeof(state));
    if (count > 0 && size_t(count) < sizeof(state) &&
        transport_.send(Kind::motor_state, terminal_notice_, state, size_t(count))) {
        terminal_notice_ = 0;
        state_notice_ = false;
    }
}
// Hardware and executor port answers run on the control worker; Core never waits here.
ayni_status_t MotorExecution::advance_owner() {
    if (mode_ == Mode::closed) {
        return AYNI_OK;
    }
    auto &mailbox = transport_.control_mailbox();
    if (const auto stop_request = mailbox.pending_stop()) {
        if (stop_request->kind == ControlStopKind::hard_estop) {
            stop_owner(stop_request);
        } else {
            const auto soft = soft_stop_owner(stop_request);
            if (soft != AYNI_OK && soft != AYNI_INTERNAL) {
                return soft;
            }
        }
    }
    apply_hold();
    // Hold on a dialogue-owned run requests an immediate soft stop. Process it before
    // another sample can emit a reduced pulse or overwrite the hold rejection reason.
    if (const auto requested = mailbox.pending_stop()) {
        if (requested->kind == ControlStopKind::hard_estop) {
            stop_owner(requested);
        } else if (const auto stopped = soft_stop_owner(requested);
                   stopped != AYNI_OK && stopped != AYNI_INTERNAL) {
            return stopped;
        }
    }
    ControlTarget target;
    if (mailbox.take_target(target)) {
        apply_target(target);
    }
    if (transport_.motor_disable_requested() && mode_ != Mode::latched && mode_ != Mode::closed) {
        admission_.disable();
        mode_ = Mode::disarmed;
        if (pending_ && pending_->phase != Phase::result) {
            end(AYNI_CALL_CANCELLED, AYNI_DENIED);
        } else if (stop_output() != AYNI_OK) {
            mode_ = Mode::latched;
        }
        publish_snapshot();
    }
    // A Core STOP queued by the previous step must win before even the first sample.
    auto &stop = requests_[AYNI_EXECUTOR_STOP - 1];
    if (stop) {
        auto request = *stop;
        stop.reset();
        const auto rc = serve(request);
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    const auto progressed = advance_pending();
    if (progressed != AYNI_OK) {
        return progressed;
    }
    // Stop requests win over a challenge/status sharing this cycle.
    constexpr size_t order[]{AYNI_EXECUTOR_CHALLENGE - 1, AYNI_EXECUTOR_RENEW - 1,
                             AYNI_EXECUTOR_STATUS - 1};
    for (const auto at : order) {
        if (!requests_[at]) {
            continue;
        }
        auto request = *requests_[at];
        requests_[at].reset();
        const auto rc = serve(request);
        if (rc != AYNI_OK) {
            return rc;
        }
    }
    publish_terminal();
    return AYNI_OK;
}
ayni_status_t MotorExecution::advance_pending() {
    if (!pending_) {
        return AYNI_OK;
    }
    auto &p = *pending_;
    const auto observation = live();
    const auto checked = ayni_control_check(gate_, now(), &observation);
    if ((p.phase == Phase::prepared || p.phase == Phase::running) &&
        (p.outcome != AYNI_CALL_COMPLETED || !observation.interlocks_ok ||
         (checked != AYNI_OK && checked != AYNI_NOT_READY))) {
        if (!observation.interlocks_ok) {
            mode_ = Mode::latched;
        }
        end(AYNI_CALL_CANCELLED, AYNI_DENIED);
    } else if (p.phase == Phase::prepared) {
        if (const auto sampled = sample(); sampled != AYNI_OK) {
            if (sampled == AYNI_INTERNAL) {
                mode_ = Mode::latched;
            }
            end(AYNI_CALL_FAILED, sampled);
        } else {
            MotorCommand hardware;
            const auto bounded = hardware_command(p.effective, hardware);
            if (bounded != AYNI_OK) {
                end(AYNI_CALL_FAILED, bounded);
                return AYNI_OK;
            }
            p.effect = Effect::possible; // Even a partially failed SDK start can have effects.
            if (output_->start(hardware) != ESP_OK) {
                mode_ = Mode::latched;
                end(AYNI_CALL_FAILED, AYNI_INTERNAL);
            } else {
                p.phase = Phase::running;
                p.waveform_deadline_us = p.deadline_us;
                const auto started_us = now();
                if (p.target_seq) {
                    report_application(started_us);
                }
                mark_output_start(p.call, started_us);
                trace_handoff("output_started", p.call, started_us);
                transport_.diagnostics().record_deferred(
                    "wifi motor_segment call=%llu seg=1/%lu gap_us=0\n",
                    static_cast<unsigned long long>(p.call),
                    static_cast<unsigned long>(p.trajectory.count));
                trace("pwm-started", AYNI_OK);
            }
        }
    } else if (p.phase == Phase::running) {
        if (p.streaming && now() >= p.deadline_us) {
            p.effect = Effect::complete; // The bounded session completed, not a fixed wheel target.
            end(AYNI_CALL_COMPLETED, AYNI_OK);
        } else if (const auto sampled = sample(); sampled != AYNI_OK) {
            if (sampled == AYNI_INTERNAL) {
                mode_ = Mode::latched;
            }
            end(AYNI_CALL_FAILED, sampled);
        } else {
            const auto state = output_->poll_segment();
            if (state == MotorOutput::Poll::completed && p.streaming) {
                p.effect = Effect::complete;
                end(AYNI_CALL_COMPLETED, AYNI_OK);
            } else if (state == MotorOutput::Poll::completed) {
                ++p.segments_completed;
                const auto done_us = now();
                ayni_control_session_step_v1 next_step{};
                next_step.struct_size = sizeof(next_step);
                const auto adv = ayni_control_session_advance_segment(session_, done_us,
                                                                      &observation, &next_step);
                if (adv != AYNI_OK) {
                    end(AYNI_CALL_CANCELLED, adv);
                } else if (next_step.state == AYNI_CONTROL_SESSION_COMPLETED) {
                    p.effect = Effect::complete;
                    end(AYNI_CALL_COMPLETED, AYNI_OK);
                } else {
                    p.segment_index = next_step.segment_index;
                    // Validate the new segment's shaped output before the hardware transition.
                    // Prepared suppresses hot replacement of the just-completed waveform.
                    p.phase = Phase::prepared;
                    const auto shaped = sample();
                    p.phase = Phase::running;
                    if (shaped != AYNI_OK) {
                        end(AYNI_CALL_CANCELLED, shaped);
                    } else if (output_->transition(p.effective) != ESP_OK) {
                        mode_ = Mode::latched;
                        end(AYNI_CALL_FAILED, AYNI_INTERNAL);
                    } else {
                        const auto gap_us = now() - done_us;
                        transport_.diagnostics().record_deferred(
                            "wifi motor_segment call=%llu seg=%lu/%lu gap_us=%llu\n",
                            static_cast<unsigned long long>(p.call),
                            static_cast<unsigned long>(p.segment_index + 1U),
                            static_cast<unsigned long>(p.trajectory.count),
                            static_cast<unsigned long long>(gap_us));
                    }
                }
            } else if (state == MotorOutput::Poll::failed) {
                mode_ = Mode::latched;
                end(AYNI_CALL_FAILED, AYNI_INTERNAL);
            }
        }
    }
    if (pending_ && pending_->phase == Phase::result) {
        const auto rc = post_result();
        if (rc != AYNI_OK && rc != AYNI_BUSY) {
            return rc;
        }
    }
    return AYNI_OK;
}
void MotorExecution::receive_owner(Kind kind, uint64_t, const uint8_t *data, size_t size) {
    if (kind == Kind::motor_settings && size == 4 && !pending_) {
        const auto limit = get32(data);
        if (limit && limit <= motor_percent_max && mode_ != Mode::latched &&
            mode_ != Mode::closed) {
            limit_ = limit;
            mode_ = Mode::armed;
        }
    } else if (kind == Kind::motor_power_off && !size) {
        stop_owner(); // Driver-off is an operator statement, never an electrical measurement.
        if (pending_ && output_->disabled()) {
            pending_->phase = Phase::result;
        }
    }
    publish_snapshot();
}
int MotorExecution::describe(char *out, size_t capacity) const {
    const auto snapshot = snapshot_.load(std::memory_order_acquire);
    const auto limit = snapshot & snapshot_limit_mask;
    const auto mode = Mode((snapshot >> snapshot_mode_shift) & snapshot_mode_mask);
    return std::snprintf(
        out, capacity,
        "{\"driver\":\"TA6586\",\"gpio_d0_d1_d2_d3\":[%lu,%lu,%lu,%lu],\"configured_supply_mv\":"
        "10000,"
        "\"power_state\":\"not-measured\",\"max_percent\":%lu,\"hard_max_percent\":%lu,\"busy\":%s,"
        "\"motion_enabled\":%s,\"mode\":\"locally-enabled-model-control\",\"stop_chain\":"
        "\"unverified\",\"control_mode\":%u}",
        (unsigned long)motor_pins[0], (unsigned long)motor_pins[1], (unsigned long)motor_pins[2],
        (unsigned long)motor_pins[3], (unsigned long)limit, (unsigned long)motor_percent_max,
        (snapshot & snapshot_busy) ? "true" : "false", mode == Mode::armed ? "true" : "false",
        unsigned(mode));
}
bool MotorExecution::close_owner() {
    if (mode_ == Mode::closed) {
        return true;
    }
    if (pending_) {
        return false;
    }
    for (const auto &request : requests_) {
        if (request) {
            return false;
        }
    }
    // The first allocation may fail before any output exists. Absence then
    // needs no stop; an acquired output still requires confirmed shutdown.
    if ((output_ && stop_output() != AYNI_OK) ||
        (session_ && ayni_control_session_destroy(session_) != AYNI_OK) ||
        (gate_ && ayni_control_destroy(gate_) != AYNI_OK)) {
        return false;
    }
    session_ = nullptr;
    gate_ = nullptr;
    if (output_ && output_->close() != ESP_OK) {
        return false;
    }
    if (output_) {
        output_->~MotorOutput();
        heap_caps_free(output_);
        output_ = nullptr;
    }
    heap_caps_free(session_memory_);
    session_memory_ = nullptr;
    heap_caps_free(gate_memory_);
    gate_memory_ = nullptr;
    mode_ = Mode::closed;
    return true;
}
} // namespace wifichat
