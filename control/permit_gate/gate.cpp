#include "gate.h"
#include <new>

using namespace ayni::control;

ayni_control_gate::ayni_control_gate(const ayni_control_config_v1 &configured, uint64_t now)
    : config(configured), last_now(now) {}
ayni_control_phase_t ayni_control_gate::phase() const {
    return !execution                         ? AYNI_CONTROL_IDLE
           : !execution->stopped              ? AYNI_CONTROL_ACTIVE
           : execution->stopped->confirmed_at ? AYNI_CONTROL_STOPPED
                                              : AYNI_CONTROL_STOPPING;
}
ayni_control_info_v1 ayni_control_gate::snapshot() const {
    ayni_control_info_v1 out{};
    out.struct_size = sizeof(out);
    out.phase = phase();
    if (execution) {
        out.permit_id = execution->permit.permit_id;
        out.operation_id = execution->permit.operation_id;
        out.deadline_us = execution->deadline_us;
        out.last_sample_sequence = execution->sample ? execution->sample->sequence : 0;
        if (execution->stopped) {
            out.stop_cause = execution->stopped->cause;
            out.stop_confirmed_at_us = execution->stopped->confirmed_at.value_or(0);
        }
    }
    return out;
}
void ayni_control_gate::stop(ayni_control_stop_cause_t cause) {
    if (phase() == AYNI_CONTROL_ACTIVE) {
        execution->stopped = Stop{cause, std::nullopt};
    }
}
ayni_status_t ayni_control_gate::tick(uint64_t now) {
    if (now < last_now || clock == Clock::regressed) {
        clock = Clock::regressed;
        stop(AYNI_CONTROL_STOP_CLOCK);
        return AYNI_STALE;
    }
    last_now = now;
    if (phase() == AYNI_CONTROL_ACTIVE && now >= execution->deadline_us) {
        stop(AYNI_CONTROL_STOP_EXPIRED);
        return AYNI_EXPIRED;
    }
    if (phase() == AYNI_CONTROL_ACTIVE && execution->sample &&
        now - execution->sample->at_us > execution->permit.max_command_age_us) {
        stop(AYNI_CONTROL_STOP_SAMPLE);
        return AYNI_EXPIRED;
    }
    return AYNI_OK;
}
ayni_status_t ayni_control_gate::observe(uint64_t now, const ayni_control_observation_v1 *live) {
    const auto time = tick(now);
    if (time != AYNI_OK) {
        return time;
    }
    if (live == nullptr || !valid_epochs(live->epochs) || live->interlocks_ok > 1 ||
        live->reserved != 0) {
        stop(AYNI_CONTROL_STOP_INTERLOCK);
        return AYNI_INVALID_ARGUMENT;
    }
    if (!same_epochs(live->epochs, config.epochs)) {
        stop(AYNI_CONTROL_STOP_EPOCH);
        return AYNI_DENIED;
    }
    if (live->interlocks_ok == 0) {
        stop(AYNI_CONTROL_STOP_INTERLOCK);
        return AYNI_DENIED;
    }
    return AYNI_OK;
}
ayni_status_t ayni_control_gate::deadline(uint64_t nonce, uint32_t lease, uint64_t now,
                                          uint64_t &out) const {
    for (const auto &entry : nonces) {
        if (entry.id != 0 && entry.id == nonce) {
            if (entry.issued > UINT64_MAX - lease) {
                return AYNI_INVALID_ARGUMENT;
            }
            const auto until = entry.issued + lease;
            if (until <= now) {
                return AYNI_EXPIRED;
            }
            out = until;
            return AYNI_OK;
        }
    }
    return AYNI_STALE;
}

// Identity/scope validation cannot alter the currently accepted operation.
// Only a canonical complete permit can replace an IDLE or measured STOPPED gate.
ayni_status_t ayni_control_gate::prepare(const ayni_execution_permit_v1 &input,
                                         ayni_execution_permit_v1 &out) const {
    const auto shape = structure(input.struct_size, sizeof(input));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (input.permit_id == 0 || input.operation_id == 0 || input.echoed_nonce == 0 ||
        input.reserved != 0 || !valid_epochs(input.epochs) || input.lease_us == 0 ||
        input.max_command_age_us == 0 ||
        (input.form != AYNI_FORM_PROCEDURE && input.form != AYNI_FORM_SESSION)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto maximum =
        input.form == AYNI_FORM_PROCEDURE ? config.procedure_max_us : config.session_lease_max_us;
    if (!same_epochs(input.epochs, config.epochs) ||
        input.control_domain_id != config.control_domain_id ||
        input.control_mode != config.control_mode || input.lease_us > maximum ||
        input.max_command_age_us > config.command_age_max_us) {
        return AYNI_DENIED;
    }
    if (execution && input.permit_id <= execution->permit.permit_id) {
        return AYNI_STALE;
    }
    auto value = input;
    const auto envelope = normalize_envelope(input.envelope, value.envelope);
    if (envelope != AYNI_OK) {
        return envelope;
    }
    if (!within(value.envelope, config.limits)) {
        return AYNI_DENIED;
    }
    out = value;
    return AYNI_OK;
}

extern "C" {
ayni_status_t ayni_control_memory_requirements(size_t *out_size, size_t *out_alignment) {
    if (out_size == nullptr || out_alignment == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out_size = sizeof(ayni_control_gate);
    *out_alignment = alignof(ayni_control_gate);
    return AYNI_OK;
}
// No allocation, constructor callback or partial ownership on failure. Startup
// accepts only explicit measured limits and nonzero generations (C-PERMIT-012).
ayni_status_t ayni_control_create(const ayni_control_config_v1 *config, uint64_t now_us,
                                  void *storage, size_t storage_size,
                                  ayni_control_gate_t **out_gate) {
    if (config == nullptr || storage == nullptr || out_gate == nullptr ||
        reinterpret_cast<uintptr_t>(storage) % alignof(ayni_control_gate) != 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = structure(config->struct_size, sizeof(*config));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (config->abi_major != AYNI_ABI_MAJOR || config->abi_minor != AYNI_ABI_MINOR) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (storage_size < sizeof(ayni_control_gate)) {
        return AYNI_BUFFER_TOO_SMALL;
    }
    if (config->control_mode == 0 || config->control_domain_id == 0 ||
        !valid_epochs(config->epochs) || config->procedure_max_us == 0 ||
        config->session_lease_max_us == 0 || config->command_age_max_us == 0 ||
        config->reserved != 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto normalized = *config;
    const auto status = normalize_envelope(config->limits, normalized.limits);
    if (status != AYNI_OK) {
        return status;
    }
    *out_gate = new (storage) ayni_control_gate(normalized, now_us);
    return AYNI_OK;
}
ayni_status_t ayni_control_destroy(ayni_control_gate_t *gate) {
    if (gate == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (gate->phase() == AYNI_CONTROL_ACTIVE || gate->phase() == AYNI_CONTROL_STOPPING) {
        return AYNI_BUSY;
    }
    gate->~ayni_control_gate();
    return AYNI_OK;
}
ayni_status_t ayni_control_nonce(ayni_control_gate_t *gate, uint64_t now_us,
                                 ayni_executor_nonce_v1 *out_nonce) {
    if (gate == nullptr || out_nonce == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto time = gate->tick(now_us);
    if (time == AYNI_STALE) {
        return time;
    }
    // Expiry stops the old operation but need not prevent a future admission
    // from obtaining a new challenge; this function never authorizes output.
    if (gate->next_nonce == UINT64_MAX) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto id = ++gate->next_nonce;
    gate->nonces[(id - 1) % AYNI_CONTROL_NONCE_COUNT] = {id, now_us};
    *out_nonce = {gate->config.control_domain_id, id};
    return AYNI_OK;
}
ayni_status_t ayni_control_start(ayni_control_gate_t *gate, const ayni_execution_permit_v1 *permit,
                                 uint64_t now_us, const ayni_control_observation_v1 *live) {
    if (gate == nullptr || permit == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto observed = gate->observe(now_us, live);
    if (observed != AYNI_OK) {
        return observed;
    }
    if (gate->phase() == AYNI_CONTROL_ACTIVE || gate->phase() == AYNI_CONTROL_STOPPING) {
        return AYNI_BUSY;
    }
    ayni_execution_permit_v1 value{};
    const auto prepared = gate->prepare(*permit, value);
    if (prepared != AYNI_OK) {
        return prepared;
    }
    uint64_t until = 0;
    const auto deadline = gate->deadline(value.echoed_nonce, value.lease_us, now_us, until);
    if (deadline != AYNI_OK) {
        return deadline;
    }
    gate->execution = ayni_control_gate::Execution{value, until, std::nullopt, std::nullopt};
    return AYNI_OK;
}
ayni_status_t ayni_control_renew(ayni_control_gate_t *gate, const ayni_permit_renewal_v1 *renewal,
                                 uint64_t now_us, const ayni_control_observation_v1 *live) {
    if (gate == nullptr || renewal == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto observed = gate->observe(now_us, live);
    if (observed != AYNI_OK) {
        return observed;
    }
    const auto shape = structure(renewal->struct_size, sizeof(*renewal));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (gate->phase() != AYNI_CONTROL_ACTIVE || gate->execution->permit.form != AYNI_FORM_SESSION ||
        renewal->permit_id != gate->execution->permit.permit_id) {
        return AYNI_INVALID_STATE;
    }
    if (renewal->reserved != 0 || !valid_epochs(renewal->epochs) || renewal->echoed_nonce == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!same_epochs(renewal->epochs, gate->execution->permit.epochs)) {
        return AYNI_DENIED;
    }
    uint64_t until = 0;
    const auto status =
        gate->deadline(renewal->echoed_nonce, gate->execution->permit.lease_us, now_us, until);
    if (status != AYNI_OK) {
        return status;
    }
    if (until > gate->execution->deadline_us) {
        gate->execution->deadline_us = until;
    }
    return AYNI_OK;
}
ayni_status_t ayni_control_check(ayni_control_gate_t *gate, uint64_t now_us,
                                 const ayni_control_observation_v1 *live) {
    if (gate == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto observed = gate->observe(now_us, live);
    return observed != AYNI_OK                    ? observed
           : gate->phase() != AYNI_CONTROL_ACTIVE ? AYNI_INVALID_STATE
           : gate->execution->sample              ? AYNI_OK
                                                  : AYNI_NOT_READY;
}
ayni_status_t ayni_control_sample(ayni_control_gate_t *gate, const ayni_control_sample_v1 *sample,
                                  uint64_t now_us, const ayni_control_observation_v1 *live) {
    if (gate == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto checked = gate->observe(now_us, live);
    if (checked != AYNI_OK) {
        return checked;
    }
    if (gate->phase() != AYNI_CONTROL_ACTIVE) {
        return AYNI_INVALID_STATE;
    }
    const auto shape =
        sample == nullptr ? AYNI_INVALID_ARGUMENT : structure(sample->struct_size, sizeof(*sample));
    if (shape != AYNI_OK) {
        gate->stop(AYNI_CONTROL_STOP_SAMPLE);
        return shape;
    }
    if (sample->permit_id != gate->execution->permit.permit_id || sample->sequence == 0 ||
        (gate->execution->sample && sample->sequence <= gate->execution->sample->sequence) ||
        sample->sampled_at_us > now_us ||
        now_us - sample->sampled_at_us > gate->execution->permit.max_command_age_us ||
        !valid_sample(*sample, gate->execution->permit)) {
        gate->stop(AYNI_CONTROL_STOP_SAMPLE);
        return AYNI_DENIED;
    }
    gate->execution->sample = ayni_control_gate::Sample{sample->sequence, sample->sampled_at_us};
    return AYNI_OK;
}
ayni_status_t ayni_control_stop(ayni_control_gate_t *gate, uint64_t permit_id, uint64_t now_us) {
    if (gate == nullptr || permit_id == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!gate->execution || permit_id != gate->execution->permit.permit_id) {
        return AYNI_INVALID_STATE;
    }
    const auto time = gate->tick(now_us);
    gate->stop(AYNI_CONTROL_STOP_REQUESTED);
    return time == AYNI_STALE ? time : AYNI_OK;
}
ayni_status_t ayni_control_confirm_stopped(ayni_control_gate_t *gate, uint64_t permit_id,
                                           uint64_t measured_at_us) {
    if (gate == nullptr || permit_id == 0) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!gate->execution || permit_id != gate->execution->permit.permit_id ||
        (gate->phase() != AYNI_CONTROL_STOPPING && gate->phase() != AYNI_CONTROL_STOPPED)) {
        return AYNI_INVALID_STATE;
    }
    if (gate->phase() == AYNI_CONTROL_STOPPED) {
        return AYNI_OK;
    }
    if (gate->clock == ayni_control_gate::Clock::valid && measured_at_us < gate->last_now) {
        return AYNI_STALE;
    }
    // A trusted fresh output-disable measurement still permits cleanup after
    // clock failure; it cannot clear that fault or authorize another permit.
    gate->execution->stopped->confirmed_at = measured_at_us;
    if (gate->clock == ayni_control_gate::Clock::valid) {
        gate->last_now = measured_at_us;
    }
    return AYNI_OK;
}
ayni_status_t ayni_control_get_info(const ayni_control_gate_t *gate,
                                    ayni_control_info_v1 *out_info) {
    if (gate == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = structure(out_info->struct_size, sizeof(*out_info));
    if (shape == AYNI_OK) {
        *out_info = gate->snapshot();
    }
    return shape;
}
} // extern C
