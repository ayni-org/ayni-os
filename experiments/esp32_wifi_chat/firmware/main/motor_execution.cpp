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
    return gate_memory_ ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
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
            motor_procedure_us,
            motor_sample_age_us,
            motor_poll_us,
            this,
            challenge_port,
            renew_port,
            stop_port,
            status_port};
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
        transport_.send(Kind::status, current_permit_, message, size_t(count));
    }
}
ayni_status_t MotorExecution::queue(const PortRequest &request) {
    if (!request.completion || request.kind < AYNI_EXECUTOR_CHALLENGE ||
        request.kind > AYNI_EXECUTOR_STATUS) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!inbox_open_.load(std::memory_order_acquire)) {
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
    const auto heartbeat = heartbeat_capture_.load(std::memory_order_acquire);
    return {epochs,
            uint32_t(accepting_.load(std::memory_order_acquire) && mode_ == Mode::armed &&
                     heartbeat && now() - heartbeat < heartbeat_us),
            0};
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
    } else if (request.kind == AYNI_EXECUTOR_STOP) {
        if (request.permit_id == current_permit_ && current_permit_) {
            if (pending_ && pending_->phase != Phase::result) {
                end(AYNI_CALL_CANCELLED, AYNI_DENIED);
            }
            response.status = stop_output();
        } else {
            response.status = AYNI_NOT_FOUND;
        }
    } else {
        response.status = status(request.permit_id, response.output.status);
    }
    if (request.kind == AYNI_EXECUTOR_CHALLENGE || response.status != AYNI_OK) {
        trace(request.kind == AYNI_EXECUTOR_CHALLENGE ? "challenge" : "executor-port",
              response.status);
    }
    const auto posted = ayni_executor_post(request.completion, &response);
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
void MotorExecution::begin(Pending incoming) {
    if (pending_ || terminal_notice_ || !gate_ || !live().interlocks_ok ||
        incoming.permit.permit_id <= refused_through_ ||
        !admission_.permits(incoming.permit.echoed_nonce) || !output_->disabled() ||
        incoming.command.first_percent < -int32_t(limit_) ||
        incoming.command.first_percent > int32_t(limit_) ||
        incoming.command.second_percent < -int32_t(limit_) ||
        incoming.command.second_percent > int32_t(limit_)) {
        reject(incoming, AYNI_DENIED);
        return;
    }
    const auto observation = live();
    const auto started = ayni_control_start(gate_, &incoming.permit, now(), &observation);
    if (started != AYNI_OK) {
        reject(incoming, started);
        return;
    }
    pending_ = incoming;
    current_permit_ = incoming.permit.permit_id;
    sequence_ = 0;
    trace("permit-accepted", AYNI_OK);
}
ayni_status_t MotorExecution::sample() {
    auto &p = *pending_;
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
    const auto rc = ayni_control_sample(gate_, &sample, now(), &observation);
    if (rc == AYNI_OK) {
        p.last_sample_us = sample.sampled_at_us;
    }
    return rc;
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
    trace("execution-end", status);
    if (stop_output() != AYNI_OK) {
        mode_ = Mode::latched;
        p.outcome = AYNI_CALL_ABANDONED;
        p.status = AYNI_RESULT_UNKNOWN;
        return; // Keep the permit/resources; retry local disable, never invent STOPPED.
    }
    // Exact local edge evidence can establish partial controller output after a known stop.
    // It does not measure wheel speed/position or repair an earlier UNKNOWN record.
    if (p.effect == Effect::possible) {
        std::array<int, 4> edges{};
        const auto duties = motor_duties(p.command.first_percent, p.command.second_percent);
        bool valid = output_->counts(edges), positive = false;
        for (size_t at = 0; at < edges.size(); ++at) {
            valid = valid && edges[at] >= 0 && uint32_t(edges[at]) <= p.command.duration_ms &&
                    (duties[at] || edges[at] == 0);
            positive = positive || edges[at] > 0;
        }
        if (valid && positive) {
            p.effect = Effect::partial;
        }
    }
    p.phase = Phase::result; // Local output-stop fact ends a timed PWM procedure; target stays
                             // unverified.
}
void MotorExecution::stop_owner() {
    if (mode_ == Mode::closed) {
        return;
    }
    mode_ = Mode::latched;
    if (pending_ && pending_->phase != Phase::result) {
        end(AYNI_CALL_CANCELLED, AYNI_DENIED);
    } else {
        stop_output();
    }
}
ayni_status_t MotorExecution::post_result() {
    auto &p = *pending_;
    char json[768];
    const auto length = std::snprintf(
        json, sizeof(json),
        "{\"permit_id\":%llu,\"motor1_percent\":%ld,\"motor2_percent\":%ld,\"duration_ms\":%lu,"
        "\"duration_basis\":\"cumulative-pwm-ms\",\"output_started\":%s,\"output_waveform_"
        "complete\":%s,\"output_stop\":\"gpio-matrix-and-"
        "low-readback\","
        "\"operator_stopped\":null,\"operator_source\":\"not-requested\",\"local_status\":%ld,"
        "\"target\":"
        "\"unverified\","
        "\"mode\":\"manual-bench-exception\",\"independent_watchdog_verified\":false}",
        (unsigned long long)current_permit_, (long)p.command.first_percent,
        (long)p.command.second_percent, (unsigned long)p.command.duration_ms,
        p.effect == Effect::none ? "false" : "true",
        p.effect == Effect::complete ? "true" : "false", (long)p.status);
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
    if (p.outcome == AYNI_CALL_ABANDONED) {
        result.status = AYNI_RESULT_UNKNOWN;
        result.effect = AYNI_CALL_EFFECT_UNKNOWN;
    } else {
        result.result = {json, uint32_t(length)};
    }
    const auto posted = ayni_provider_post(p.completion, &result);
    if (posted == AYNI_OK || posted == AYNI_INVALID_STATE) {
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
    publish_terminal();
    if (transport_.motor_stop_requested() ||
        local_stop_.exchange(false, std::memory_order_acq_rel)) {
        stop_owner();
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
        if (sample() != AYNI_OK) {
            end(AYNI_CALL_FAILED, AYNI_DENIED);
        } else {
            p.effect = Effect::possible; // Even a partially failed SDK start can have effects.
            if (output_->start(p.command) != ESP_OK) {
                mode_ = Mode::latched;
                end(AYNI_CALL_FAILED, AYNI_INTERNAL);
            } else {
                p.phase = Phase::running;
                trace("pwm-started", AYNI_OK);
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
        (gate_ && ayni_control_destroy(gate_) != AYNI_OK)) {
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
} // namespace wifichat
