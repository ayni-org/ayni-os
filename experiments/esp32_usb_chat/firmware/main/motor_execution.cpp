#include "motor_execution.h"
#include "motor_contract.h"
#include <algorithm>
#include <ayni/values.hpp>
#include <cstdio>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <new>
namespace usbchat {
namespace {
constexpr uint64_t review_us = 30000000, heartbeat_us = 3000000;
uint64_t now() {
    return uint64_t(esp_timer_get_time());
}
bool same(const ayni_permit_epochs_v1 &a, const ayni_permit_epochs_v1 &b) {
    return a.kernel_boot_epoch == b.kernel_boot_epoch && a.authority_epoch == b.authority_epoch &&
           a.executor_boot_epoch == b.executor_boot_epoch && a.safety_epoch == b.safety_epoch &&
           a.calibration_version == b.calibration_version && a.reserved == b.reserved;
}
} // namespace
ayni_status_t MotorExecution::initialize() {
    executor_boot_ = (uint64_t(esp_random()) << 32) | esp_random();
    if (!executor_boot_) {
        executor_boot_ = 1;
    }
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
    return gate_memory_ ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
ayni_executor_v1 MotorExecution::ports() {
    return {sizeof(ayni_executor_v1),
            motor_version,
            motor_text("motor.executor"),
            motor_domain,
            executor_boot_,
            1,
            1,
            1,
            motor_procedure_us,
            motor_procedure_us,
            motor_sample_age_us,
            motor_poll_us,
            this,
            challenge_port,
            renew_port,
            stop_port,
            status_port};
}
ayni_status_t MotorExecution::queue(const PortRequest &request) {
    if (!request.completion || request.kind < AYNI_EXECUTOR_CHALLENGE ||
        request.kind > AYNI_EXECUTOR_STATUS) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &slot = requests_[request.kind - 1];
    if (slot) {
        return AYNI_BUSY;
    }
    slot = request;
    return AYNI_OK;
}
ayni_status_t MotorExecution::challenge_port(void *ctx, ayni_attempt_ref_v1 id,
                                             const ayni_permit_epochs_v1 *epochs,
                                             ayni_executor_completion_t *completion) {
    if (!epochs) {
        return AYNI_INVALID_ARGUMENT;
    }
    return static_cast<MotorExecution *>(ctx)->queue(
        {AYNI_EXECUTOR_CHALLENGE, id, completion, *epochs, 0});
}
ayni_status_t MotorExecution::renew_port(void *, ayni_attempt_ref_v1,
                                         const ayni_permit_renewal_v1 *,
                                         ayni_executor_completion_t *) {
    return AYNI_INVALID_STATE; // Only bounded, nonrenewable procedures are registered.
}
ayni_status_t MotorExecution::stop_port(void *ctx, ayni_attempt_ref_v1 id, uint64_t permit,
                                        ayni_executor_completion_t *completion) {
    return static_cast<MotorExecution *>(ctx)->queue(
        {AYNI_EXECUTOR_STOP, id, completion, {}, permit});
}
ayni_status_t MotorExecution::status_port(void *ctx, ayni_attempt_ref_v1 id, uint64_t permit,
                                          ayni_executor_completion_t *completion) {
    return static_cast<MotorExecution *>(ctx)->queue(
        {AYNI_EXECUTOR_STATUS, id, completion, {}, permit});
}
ayni_control_observation_v1 MotorExecution::live() const {
    auto epochs = epochs_;
    if (mode_ == Mode::latched) {
        ++epochs.safety_epoch;
    }
    return {epochs,
            uint32_t(accepting_ && mode_ == Mode::armed && heartbeat_us_ &&
                     now() - heartbeat_us_ < heartbeat_us),
            0};
}
bool MotorExecution::scene_enabled() const {
    return mode_ == Mode::armed;
}
ayni_status_t MotorExecution::challenge(const ayni_permit_epochs_v1 &expected,
                                        ayni_executor_nonce_v1 &nonce) {
    if (!live().interlocks_ok || pending_ || expected.executor_boot_epoch != executor_boot_ ||
        expected.safety_epoch != 1 || expected.calibration_version != 1) {
        return AYNI_DENIED;
    }
    if (!gate_) {
        const ayni_control_config_v1 config{
            sizeof(config),     AYNI_ABI_MAJOR,     AYNI_ABI_MINOR,      1, motor_domain,  expected,
            motor_procedure_us, motor_procedure_us, motor_sample_age_us, 0, motor_limits()};
        auto rc = ayni_control_create(&config, now(), gate_memory_, gate_bytes_, &gate_);
        if (rc != AYNI_OK) {
            return rc;
        }
        epochs_ = expected; // Trusted Core boot is bound once for this executor lifetime.
    } else if (!same(epochs_, expected)) {
        return AYNI_STALE;
    }
    return ayni_control_nonce(gate_, now(), &nonce);
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
    // NOT_READY before the first operator-approved sample still has an ACTIVE gate.
    return ayni_control_get_info(gate_, &out.control);
}
ayni_status_t MotorExecution::serve(PortRequest &request) {
    ayni_executor_response_v1 response{};
    response.struct_size = sizeof(response);
    response.kind = request.kind;
    response.request = request.identity;
    if (request.kind == AYNI_EXECUTOR_CHALLENGE) {
        response.status = challenge(request.epochs, response.output.nonce);
    } else if (request.kind == AYNI_EXECUTOR_STOP) {
        if (request.permit_id == current_permit_ && current_permit_) {
            if (pending_ && pending_->phase != Phase::observe && pending_->phase != Phase::result) {
                end(AYNI_CALL_CANCELLED, AYNI_DENIED);
            }
            response.status = stop_output();
        } else {
            response.status = AYNI_NOT_FOUND;
        }
    } else {
        response.status = status(request.permit_id, response.output.status);
    }
    const auto posted = ayni_executor_post(request.completion, &response);
    // Core may have abandoned this exact answer after its bounded wait. Never replay it.
    return posted == AYNI_INVALID_STATE ? AYNI_OK : posted;
}
ayni_submit_result_t MotorExecution::submit(const ayni_provider_request_v1 *request,
                                            ayni_provider_completion_t *completion) {
    if (!request || !completion || pending_ || terminal_notice_ || !gate_ ||
        !live().interlocks_ok || !request->execution_permit ||
        request->execution_permit->permit_id <= refused_through_ ||
        request->execution_permit->operation_id != request->operation_id || !output_->disabled()) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    ayni::ValueReader reader;
    if (reader.read(request->value) != AYNI_OK) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    int64_t values[3]{};
    for (uint32_t at = 0; at < 3; ++at) {
        const auto *field = reader.field(at + 1);
        if (!field || field->kind != AYNI_VALUE_I64) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        values[at] = field->value.integer;
    }
    // Validate before narrowing; values also have exact Core-authorized envelope mappings.
    if (values[0] < -int64_t(limit_) || values[0] > int64_t(limit_) ||
        values[1] < -int64_t(limit_) || values[1] > int64_t(limit_) ||
        values[2] < motor_duration_ms_min || values[2] > motor_plan_duration_ms_max) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const MotorCommand command{int32_t(values[0]), int32_t(values[1]), uint32_t(values[2])};
    if (!valid_motor_command(command) || (!command.first_percent && !command.second_percent) ||
        review_nonce_ == UINT64_MAX) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    const auto observation = live();
    if (ayni_control_start(gate_, request->execution_permit, now(), &observation) != AYNI_OK) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    pending_.emplace();
    auto &p = *pending_;
    p.attempt = request->attempt;
    p.completion = completion;
    p.permit = *request->execution_permit;
    p.identity = {++review_nonce_, request->logical_call_id, request->operation_id,
                  request->attempt.boot_epoch, request->attempt.request_id};
    p.command = command;
    p.review_deadline_us = now() + review_us;
    current_permit_ = p.permit.permit_id;
    sequence_ = 0;
    return AYNI_SUBMIT_ACCEPTED; // No output or blocking UI inside the Core callback.
}
ayni_submit_result_t MotorExecution::cancel(ayni_attempt_ref_v1 attempt) {
    if (!pending_ || attempt.boot_epoch != pending_->attempt.boot_epoch ||
        attempt.request_id != pending_->attempt.request_id) {
        return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
    }
    pending_->outcome = AYNI_CALL_CANCELLED;
    pending_->status = AYNI_DENIED;
    return AYNI_SUBMIT_ACCEPTED;
}
ayni_status_t MotorExecution::sample() {
    const auto &p = *pending_;
    ayni_control_sample_v1 sample{};
    sample.struct_size = sizeof(sample);
    sample.count = p.permit.envelope.count;
    sample.permit_id = p.permit.permit_id;
    sample.sequence = ++sequence_;
    sample.sampled_at_us = now();
    const auto limits = motor_limits();
    const int64_t values[]{p.command.first_percent, p.command.second_percent,
                           p.command.duration_ms};
    sample.count = limits.count;
    for (uint32_t at = 0; at < limits.count; ++at) {
        sample.fields[at] = {limits.fields[at].field_id, limits.fields[at].unit, values[at]};
    }
    const auto observation = live();
    return ayni_control_sample(gate_, &sample, now(), &observation);
}
ayni_status_t MotorExecution::stop_output() {
    ayni_status_t stopped = AYNI_OK;
    if (gate_ && current_permit_) {
        stopped = ayni_control_stop(gate_, current_permit_, now());
    }
    if (!output_ || output_->stop() != ESP_OK || !output_->disabled()) {
        return AYNI_INTERNAL;
    }
    if (stopped != AYNI_OK) {
        return stopped;
    }
    if (!gate_ || !current_permit_) {
        return AYNI_OK;
    }
    // C-STOP-006: actual controller output readback, separate from wheel/target facts.
    return ayni_control_confirm_stopped(gate_, current_permit_, now());
}
void MotorExecution::end(ayni_call_outcome_t outcome, ayni_status_t status) {
    if (!pending_) {
        return;
    }
    auto &p = *pending_;
    p.outcome = outcome;
    p.status = status;
    if (stop_output() != AYNI_OK) {
        mode_ = Mode::latched;
        p.outcome = AYNI_CALL_ABANDONED;
        p.status = AYNI_RESULT_UNKNOWN;
        return; // Keep the permit/resources; retry local disable, never invent STOPPED.
    }
    if (p.effect == Effect::none) {
        p.phase = Phase::result;
    } else {
        p.phase = Phase::observe_unsent;
        p.review_deadline_us = now() + review_us;
    }
}
void MotorExecution::request_stop() {
    if (mode_ == Mode::closed) {
        return;
    }
    mode_ = Mode::latched;
    if (pending_ && pending_->phase != Phase::observe && pending_->phase != Phase::observe_unsent &&
        pending_->phase != Phase::result) {
        end(AYNI_CALL_CANCELLED, AYNI_DENIED);
    } else {
        stop_output();
    }
}
bool MotorExecution::notify() {
    if (!pending_) {
        return true;
    }
    auto &p = *pending_;
    const bool review = p.phase == Phase::review_unsent;
    if (!review && p.phase != Phase::observe_unsent) {
        return true;
    }
    uint8_t packet[motor_review_bytes]{};
    encode_motor_review(packet, p.identity, p.command, limit_, review ? 1 : 2);
    if (!transport_.send(review ? Kind::motor_review : Kind::motor_observe, current_permit_, packet,
                         sizeof(packet))) {
        return false;
    }
    p.phase = review ? Phase::review : Phase::observe;
    return true;
}
ayni_status_t MotorExecution::post_result() {
    auto &p = *pending_;
    char json[768];
    const auto length = std::snprintf(
        json, sizeof(json),
        "{\"permit_id\":%llu,\"motor1_percent\":%ld,\"motor2_percent\":%ld,\"duration_ms\":%lu,"
        "\"output_started\":%s,\"output_waveform_complete\":%s,\"output_stop\":\"gpio-matrix-and-"
        "low-readback\","
        "\"operator_stopped\":%s,\"operator_source\":\"mac-human\",\"local_status\":%ld,\"target\":"
        "\"unverified\","
        "\"mode\":\"manual-bench-exception\",\"independent_watchdog_verified\":false}",
        (unsigned long long)current_permit_, (long)p.command.first_percent,
        (long)p.command.second_percent, (unsigned long)p.command.duration_ms,
        p.effect == Effect::none ? "false" : "true",
        p.effect == Effect::complete ? "true" : "false", p.operator_quiet ? "true" : "false",
        (long)p.status);
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
                                                   : AYNI_CALL_EFFECT_UNKNOWN;
    if (p.outcome == AYNI_CALL_ABANDONED) {
        result.status = AYNI_RESULT_UNKNOWN;
        result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    } else {
        result.result = {json, uint32_t(length)};
    }
    const auto posted = ayni_provider_post(p.completion, &result);
    if (posted == AYNI_OK || posted == AYNI_INVALID_STATE) {
        pending_.reset();
        terminal_notice_ = current_permit_;
        publish_terminal();
        return AYNI_OK;
    }
    return posted;
}
void MotorExecution::publish_terminal() {
    if (!terminal_notice_) {
        return;
    }
    char state[512];
    const auto count = describe(state, sizeof(state));
    if (count > 0 && size_t(count) < sizeof(state) &&
        transport_.send(Kind::motor_state, terminal_notice_, state, size_t(count))) {
        terminal_notice_ = 0;
    }
}
// All SDK work and port answers run between bounded Core steps on the owner thread.
ayni_status_t MotorExecution::advance() {
    if (mode_ == Mode::closed) {
        return AYNI_OK;
    }
    publish_terminal();
    if (transport_.motor_stop_requested() || local_stop_) {
        local_stop_ = false;
        request_stop();
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
    return AYNI_OK;
}
ayni_status_t MotorExecution::advance_pending() {
    if (!pending_) {
        return AYNI_OK;
    }
    auto &p = *pending_;
    const auto observation = live();
    const auto checked = ayni_control_check(gate_, now(), &observation);
    if ((p.phase == Phase::review || p.phase == Phase::review_unsent ||
         p.phase == Phase::approved || p.phase == Phase::running) &&
        (p.outcome != AYNI_CALL_COMPLETED || !observation.interlocks_ok ||
         (checked != AYNI_OK && checked != AYNI_NOT_READY))) {
        if (!observation.interlocks_ok) {
            mode_ = Mode::latched;
        }
        end(AYNI_CALL_CANCELLED, AYNI_DENIED);
    } else if (p.phase == Phase::review && now() >= p.review_deadline_us) {
        end(AYNI_CALL_CANCELLED, AYNI_EXPIRED);
    } else if (p.phase == Phase::approved) {
        if (sample() != AYNI_OK) {
            end(AYNI_CALL_FAILED, AYNI_DENIED);
        } else {
            p.effect = Effect::possible; // Even a partially failed SDK start can have effects.
            if (output_->start(p.command) != ESP_OK) {
                mode_ = Mode::latched;
                end(AYNI_CALL_FAILED, AYNI_INTERNAL);
            } else {
                p.phase = Phase::running;
            }
        }
    } else if (p.phase == Phase::running) {
        if (sample() != AYNI_OK) {
            end(AYNI_CALL_CANCELLED, AYNI_EXPIRED);
        } else {
            const auto state = output_->poll();
            if (state == MotorOutput::Poll::completed) {
                p.effect = Effect::complete;
                end(AYNI_CALL_COMPLETED, AYNI_OK);
            } else if (state == MotorOutput::Poll::failed) {
                mode_ = Mode::latched;
                end(AYNI_CALL_FAILED, AYNI_INTERNAL);
            }
        }
    } else if ((p.phase == Phase::observe || p.phase == Phase::observe_unsent) &&
               now() >= p.review_deadline_us) {
        mode_ = Mode::latched;
        p.outcome = AYNI_CALL_ABANDONED;
        p.status = AYNI_RESULT_UNKNOWN;
        p.phase = Phase::result;
    }
    notify();
    if (pending_ && pending_->phase == Phase::result) {
        const auto rc = post_result();
        if (rc != AYNI_OK && rc != AYNI_BUSY) {
            return rc;
        }
    }
    return AYNI_OK;
}
void MotorExecution::receive(Kind kind, uint64_t id, const uint8_t *data, size_t size) {
    if (kind == Kind::motor_heartbeat && !size) {
        heartbeat_us_ = now();
        return;
    }
    if (kind == Kind::motor_settings && size == 4 && !pending_) {
        const auto limit = get32(data);
        if (limit && limit <= motor_percent_max && mode_ != Mode::latched &&
            mode_ != Mode::closed) {
            limit_ = limit;
            mode_ = Mode::armed;
        }
    } else if (kind == Kind::motor_power_off && !size) {
        request_stop(); // Driver-off is an operator statement, never an electrical measurement.
        if (pending_ && output_->disabled()) {
            pending_->operator_quiet = true;
            pending_->phase = Phase::result;
        }
    } else if (pending_ && id == current_permit_) {
        auto &p = *pending_;
        bool approved = false;
        if (!motor_decision_matches(data, size, p.identity, approved)) {
            return;
        }
        if (kind == Kind::motor_decision && p.phase == Phase::review &&
            now() < p.review_deadline_us) {
            if (approved) {
                p.phase = Phase::approved;
            } else {
                end(AYNI_CALL_CANCELLED, AYNI_DENIED);
            }
        } else if (kind == Kind::motor_observed && p.phase == Phase::observe &&
                   now() < p.review_deadline_us) {
            p.operator_quiet = approved;
            if (!approved) {
                mode_ = Mode::latched;
                p.outcome = AYNI_CALL_ABANDONED;
                p.status = AYNI_RESULT_UNKNOWN;
            }
            p.phase = Phase::result;
        }
    }
    char state[512];
    const auto count = describe(state, sizeof(state));
    if (count > 0 && size_t(count) < sizeof(state)) {
        transport_.send(Kind::motor_state, current_permit_, state, size_t(count));
    }
}
int MotorExecution::describe(char *out, size_t capacity) const {
    return std::snprintf(
        out, capacity,
        "{\"driver\":\"TA6586\",\"gpio_d0_d1_d2_d3\":[%lu,%lu,%lu,%lu],\"configured_supply_mv\":"
        "10000,"
        "\"power_state\":\"not-measured\",\"max_percent\":%lu,\"hard_max_percent\":%lu,\"busy\":%s,"
        "\"motion_enabled\":%s,\"mode\":\"manual-bench-per-action-approval\",\"stop_chain\":"
        "\"unverified\"}",
        (unsigned long)motor_pins[0], (unsigned long)motor_pins[1], (unsigned long)motor_pins[2],
        (unsigned long)motor_pins[3], (unsigned long)limit_, (unsigned long)motor_percent_max,
        pending_ ? "true" : "false", mode_ == Mode::armed ? "true" : "false");
}
bool MotorExecution::close() {
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
    if (stop_output() != AYNI_OK || (gate_ && ayni_control_destroy(gate_) != AYNI_OK)) {
        return false;
    }
    gate_ = nullptr;
    if (output_ && output_->close() != ESP_OK) {
        return false;
    }
    if (output_) {
        output_->~MotorOutput();
        heap_caps_free(output_);
        output_ = nullptr;
    }
    heap_caps_free(gate_memory_);
    gate_memory_ = nullptr;
    mode_ = Mode::closed;
    return true;
}
} // namespace usbchat
