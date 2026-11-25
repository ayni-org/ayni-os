#include "session.h"
#include <cstring>
#include <new>

using namespace ayni::control;

namespace {
// Reuses permit_gate's single sample validator so ControlSession and PermitGate never diverge.
bool segment_within_permit(const ayni_control_segment_v1 &segment,
                           const ayni_execution_permit_v1 &permit) {
    if (structure(segment.struct_size, sizeof(segment)) != AYNI_OK ||
        segment.field_count != permit.envelope.count) {
        return false;
    }
    ayni_control_sample_v1 probe{};
    probe.struct_size = sizeof(probe);
    probe.count = segment.field_count;
    probe.permit_id = permit.permit_id;
    probe.sequence = 1;
    for (uint32_t at = 0; at < segment.field_count; ++at) {
        probe.fields[at] = segment.fields[at];
    }
    return valid_sample(probe, permit);
}

// An externally shaped setpoint may only narrow each corresponding signed value.
// PermitGate still validates the actual narrowed values against the authorized envelope.
bool tightens(const ayni_control_segment_v1 &effective, const ayni_control_segment_v1 &desired) {
    if (structure(effective.struct_size, sizeof(effective)) != AYNI_OK ||
        effective.field_count != desired.field_count ||
        effective.duration_us != desired.duration_us) {
        return false;
    }
    for (uint32_t at = 0; at < desired.field_count; ++at) {
        const auto &source = desired.fields[at];
        const ayni_control_value_v1 *value = nullptr;
        for (uint32_t find = 0; find < effective.field_count; ++find) {
            if (compare_names(effective.fields[find].field_id, source.field_id) == 0) {
                if (value) {
                    return false;
                }
                value = &effective.fields[find];
            }
        }
        if (!value || compare_names(value->unit, source.unit) != 0 ||
            (source.value >= 0 ? value->value < 0 || value->value > source.value
                               : value->value > 0 || value->value < source.value)) {
            return false;
        }
    }
    return true;
}

// Saturating elapsed check avoids uint64_t overflow on large duration_us values.
bool segment_elapsed(uint64_t now_us, uint64_t started_us, uint64_t duration_us) {
    return duration_us != 0U && now_us >= started_us && (now_us - started_us) >= duration_us;
}

uint64_t remaining_duration(uint64_t now_us, uint64_t started_us, uint64_t duration_us) {
    if (duration_us == 0U) {
        return 0U;
    }
    if (now_us <= started_us) {
        return duration_us;
    }
    const uint64_t elapsed = now_us - started_us;
    return elapsed >= duration_us ? 0U : duration_us - elapsed;
}
} // namespace

bool ayni_control_session::valid_segment(const ayni_control_segment_v1 &segment) const {
    return segment_within_permit(segment, permit);
}

ayni_status_t ayni_control_session::emit_sample(uint64_t now_us,
                                                const ayni_control_observation_v1 *live,
                                                const ayni_control_segment_v1 &seg) {
    if (sequence == UINT64_MAX) {
        state = AYNI_CONTROL_SESSION_STOPPING;
        static_cast<void>(ayni_control_stop(gate, permit.permit_id, now_us));
        return AYNI_CAPACITY_EXCEEDED;
    }
    ayni_control_sample_v1 sample{};
    sample.struct_size = sizeof(sample);
    sample.count = seg.field_count;
    sample.permit_id = permit.permit_id;
    sample.sequence = ++sequence;
    sample.sampled_at_us = now_us;
    for (uint32_t at = 0; at < seg.field_count; ++at) {
        sample.fields[at] = seg.fields[at];
    }
    const auto rc = ayni_control_sample(gate, &sample, now_us, live);
    if (rc != AYNI_OK) {
        state = AYNI_CONTROL_SESSION_STOPPING;
    }
    return rc;
}

void ayni_control_session::describe_step(uint64_t now_us, bool transitioned,
                                         ayni_control_session_step_v1 &out_step) const {
    out_step = {};
    out_step.struct_size = sizeof(out_step);
    out_step.state = state;
    out_step.segment_index = segment_index;
    out_step.segment_count = segment_count;
    out_step.transitioned = transitioned ? 1U : 0U;
    if (state == AYNI_CONTROL_SESSION_ACTIVE && segment_index < segment_count) {
        out_step.active = segments[segment_index];
        out_step.remaining_us =
            remaining_duration(now_us, segment_started_us, out_step.active.duration_us);
    }
}

extern "C" {
ayni_status_t ayni_control_session_memory_requirements(size_t *out_size, size_t *out_alignment) {
    if (out_size == nullptr || out_alignment == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    *out_size = sizeof(ayni_control_session);
    *out_alignment = alignof(ayni_control_session);
    return AYNI_OK;
}

ayni_status_t ayni_control_session_create(void *storage, size_t storage_size,
                                          ayni_control_session_t **out_session) {
    if (storage == nullptr || out_session == nullptr ||
        reinterpret_cast<uintptr_t>(storage) % alignof(ayni_control_session) != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (storage_size < sizeof(ayni_control_session)) {
        return AYNI_BUFFER_TOO_SMALL;
    }
    *out_session = new (storage) ayni_control_session();
    return AYNI_OK;
}

ayni_status_t ayni_control_session_destroy(ayni_control_session_t *session) {
    if (session == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (session->state != AYNI_CONTROL_SESSION_IDLE &&
        session->state != AYNI_CONTROL_SESSION_STOPPED) {
        return AYNI_BUSY;
    }
    session->~ayni_control_session();
    return AYNI_OK;
}

// Validates all initial segments against the normalized permit BEFORE calling ayni_control_start,
// so malformed or out-of-envelope trajectories never arm the gate (ADR-0112).
ayni_status_t ayni_control_session_open(ayni_control_session_t *session, ayni_control_gate_t *gate,
                                        const ayni_execution_permit_v1 *permit,
                                        const ayni_control_segment_v1 *segments,
                                        uint32_t segment_count, uint64_t now_us,
                                        const ayni_control_observation_v1 *live) {
    if (session == nullptr || gate == nullptr || permit == nullptr || segments == nullptr ||
        segment_count == 0U || segment_count > AYNI_CONTROL_SESSION_SEGMENTS_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (session->state != AYNI_CONTROL_SESSION_IDLE &&
        session->state != AYNI_CONTROL_SESSION_STOPPED) {
        return AYNI_BUSY;
    }
    if (permit->form == AYNI_FORM_PROCEDURE && segment_count != 1U) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_execution_permit_v1 prepared{};
    const auto prep = gate->prepare(*permit, prepared);
    if (prep != AYNI_OK) {
        return prep;
    }
    for (uint32_t at = 0; at < segment_count; ++at) {
        if (!segment_within_permit(segments[at], prepared)) {
            return AYNI_DENIED;
        }
    }
    const auto started = ayni_control_start(gate, permit, now_us, live);
    if (started != AYNI_OK) {
        return started;
    }
    session->gate = gate;
    session->permit = prepared;
    session->state = AYNI_CONTROL_SESSION_ACTIVE;
    session->segment_count = segment_count;
    session->segment_index = 0;
    session->segment_started_us = now_us;
    session->sequence = 0;
    session->transitions = 0;
    session->target_replaced = false;
    session->first_sample_emitted = false;
    for (uint32_t at = 0; at < segment_count; ++at) {
        session->segments[at] = segments[at];
    }
    return AYNI_OK;
}

ayni_status_t ayni_control_session_push_segment(ayni_control_session_t *session,
                                                const ayni_control_segment_v1 *segment) {
    if (session == nullptr || segment == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (session->state != AYNI_CONTROL_SESSION_ACTIVE ||
        session->permit.form != AYNI_FORM_SESSION) {
        return AYNI_INVALID_STATE;
    }
    if (!session->valid_segment(*segment)) {
        return AYNI_DENIED;
    }
    // Compact consumed prefix when tail reaches end so long streaming sessions never exhaust slots.
    if (session->segment_count == AYNI_CONTROL_SESSION_SEGMENTS_MAX) {
        if (session->segment_index == 0U) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        const uint32_t remaining = session->segment_count - session->segment_index;
        for (uint32_t at = 0; at < remaining; ++at) {
            session->segments[at] = session->segments[session->segment_index + at];
        }
        session->segment_index = 0;
        session->segment_count = remaining;
    }
    session->segments[session->segment_count++] = *segment;
    return AYNI_OK;
}

ayni_status_t ayni_control_session_replace_target(ayni_control_session_t *session,
                                                  const ayni_control_segment_v1 *target,
                                                  uint64_t now_us) {
    if (session == nullptr || target == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (session->state != AYNI_CONTROL_SESSION_ACTIVE ||
        session->permit.form != AYNI_FORM_SESSION) {
        return AYNI_INVALID_STATE;
    }
    if (!session->valid_segment(*target)) {
        return AYNI_DENIED;
    }
    session->segments[0] = *target;
    session->segment_index = 0;
    session->segment_count = 1;
    session->segment_started_us = now_us;
    session->target_replaced = true;
    ++session->transitions;
    return AYNI_OK;
}

ayni_status_t ayni_control_session_renew(ayni_control_session_t *session,
                                         const ayni_permit_renewal_v1 *renewal, uint64_t now_us,
                                         const ayni_control_observation_v1 *live) {
    if (session == nullptr || renewal == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (session->state != AYNI_CONTROL_SESSION_ACTIVE || session->gate == nullptr) {
        return AYNI_INVALID_STATE;
    }
    const auto rc = ayni_control_renew(session->gate, renewal, now_us, live);
    if (rc != AYNI_OK && session->gate->phase() != AYNI_CONTROL_ACTIVE) {
        session->state = AYNI_CONTROL_SESSION_STOPPING;
    }
    return rc;
}

// Advances elapsed time-bounded segments seamlessly in the same tick and validates the active
// setpoint via ayni_control_sample() so every cycle stays inside permit/lease/interlock bounds.
static ayni_status_t tick_session(ayni_control_session_t *session, uint64_t now_us,
                                  const ayni_control_observation_v1 *live,
                                  ayni_control_session_step_v1 *out_step,
                                  const ayni_control_segment_v1 *effective) {
    if (session == nullptr || out_step == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = structure(out_step->struct_size, sizeof(*out_step));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (session->state != AYNI_CONTROL_SESSION_ACTIVE || session->gate == nullptr) {
        session->describe_step(now_us, false, *out_step);
        return AYNI_INVALID_STATE;
    }
    bool transitioned = session->target_replaced || !session->first_sample_emitted;
    session->target_replaced = false;
    while (session->first_sample_emitted &&
           segment_elapsed(now_us, session->segment_started_us,
                           session->segments[session->segment_index].duration_us)) {
        const uint64_t elapsed_end =
            session->segment_started_us + session->segments[session->segment_index].duration_us;
        if (session->segment_index + 1U < session->segment_count) {
            ++session->segment_index;
            ++session->transitions;
            session->segment_started_us = elapsed_end;
            transitioned = true;
        } else {
            // Re-verify gate clock, lease deadline, epochs, and interlocks before reporting
            // COMPLETED so end-of-trajectory faults stay fail-closed with their true stop_cause.
            const auto obs = session->gate->observe(now_us, live);
            if (obs != AYNI_OK || session->gate->phase() != AYNI_CONTROL_ACTIVE) {
                session->state = AYNI_CONTROL_SESSION_STOPPING;
                session->describe_step(now_us, false, *out_step);
                return obs != AYNI_OK ? obs : AYNI_INVALID_STATE;
            }
            session->state = AYNI_CONTROL_SESSION_COMPLETED;
            session->describe_step(now_us, false, *out_step);
            return AYNI_OK;
        }
    }
    const auto &desired = session->segments[session->segment_index];
    if (effective && !tightens(*effective, desired)) {
        session->state = AYNI_CONTROL_SESSION_STOPPING;
        static_cast<void>(ayni_control_stop(session->gate, session->permit.permit_id, now_us));
        session->describe_step(now_us, false, *out_step);
        return AYNI_DENIED;
    }
    const auto rc = session->emit_sample(now_us, live, effective ? *effective : desired);
    if (rc == AYNI_OK) {
        session->first_sample_emitted = true;
    }
    session->describe_step(now_us, rc == AYNI_OK && transitioned, *out_step);
    if (rc == AYNI_OK && effective) {
        out_step->active = *effective;
    }
    return rc;
}

ayni_status_t ayni_control_session_tick(ayni_control_session_t *session, uint64_t now_us,
                                        const ayni_control_observation_v1 *live,
                                        ayni_control_session_step_v1 *out_step) {
    return tick_session(session, now_us, live, out_step, nullptr);
}
ayni_status_t ayni_control_session_tick_effective(ayni_control_session_t *session, uint64_t now_us,
                                                  const ayni_control_observation_v1 *live,
                                                  const ayni_control_segment_v1 *effective,
                                                  ayni_control_session_step_v1 *out_step) {
    if (!effective) {
        return AYNI_INVALID_ARGUMENT;
    }
    // Caller may reuse out_step.active as its override. Retain the validated value before
    // describe_step writes the output, so hardware never receives the unshaped desired value.
    const auto retained = *effective;
    return tick_session(session, now_us, live, out_step, &retained);
}

// Advances immediately on hardware segment completion (e.g. RMT PWM finish) without wall-clock
// jitter, emitting the next segment's gate sample atomically in the same control cycle.
ayni_status_t ayni_control_session_advance_segment(ayni_control_session_t *session, uint64_t now_us,
                                                   const ayni_control_observation_v1 *live,
                                                   ayni_control_session_step_v1 *out_step) {
    if (session == nullptr || out_step == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = structure(out_step->struct_size, sizeof(*out_step));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (session->state != AYNI_CONTROL_SESSION_ACTIVE || session->gate == nullptr) {
        session->describe_step(now_us, false, *out_step);
        return AYNI_INVALID_STATE;
    }
    if (session->segment_index + 1U >= session->segment_count) {
        const auto obs = session->gate->observe(now_us, live);
        if (obs != AYNI_OK || session->gate->phase() != AYNI_CONTROL_ACTIVE) {
            session->state = AYNI_CONTROL_SESSION_STOPPING;
            session->describe_step(now_us, false, *out_step);
            return obs != AYNI_OK ? obs : AYNI_INVALID_STATE;
        }
        session->state = AYNI_CONTROL_SESSION_COMPLETED;
        session->describe_step(now_us, false, *out_step);
        return AYNI_OK;
    }
    ++session->segment_index;
    ++session->transitions;
    session->segment_started_us = now_us;
    session->target_replaced = false;
    const auto rc = session->emit_sample(now_us, live, session->segments[session->segment_index]);
    if (rc == AYNI_OK) {
        session->first_sample_emitted = true;
    }
    session->describe_step(now_us, rc == AYNI_OK, *out_step);
    return rc;
}

ayni_status_t ayni_control_session_soft_stop(ayni_control_session_t *session, uint64_t now_us) {
    if (session == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (session->gate == nullptr || session->state == AYNI_CONTROL_SESSION_IDLE) {
        return AYNI_INVALID_STATE;
    }
    if (session->state == AYNI_CONTROL_SESSION_STOPPED) {
        return AYNI_OK;
    }
    session->state = AYNI_CONTROL_SESSION_STOPPING;
    return ayni_control_stop(session->gate, session->permit.permit_id, now_us);
}

ayni_status_t ayni_control_session_confirm_stopped(ayni_control_session_t *session,
                                                   uint64_t measured_at_us) {
    if (session == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (session->gate == nullptr || (session->state != AYNI_CONTROL_SESSION_STOPPING &&
                                     session->state != AYNI_CONTROL_SESSION_COMPLETED &&
                                     session->state != AYNI_CONTROL_SESSION_STOPPED)) {
        return AYNI_INVALID_STATE;
    }
    if (session->state == AYNI_CONTROL_SESSION_STOPPED) {
        return AYNI_OK;
    }
    // Ensure the gate has entered STOPPING before confirming local hardware readback.
    if (session->gate->phase() == AYNI_CONTROL_ACTIVE) {
        const auto stop_rc =
            ayni_control_stop(session->gate, session->permit.permit_id, measured_at_us);
        if (stop_rc != AYNI_OK) {
            return stop_rc;
        }
    }
    const auto rc =
        ayni_control_confirm_stopped(session->gate, session->permit.permit_id, measured_at_us);
    if (rc == AYNI_OK) {
        session->state = AYNI_CONTROL_SESSION_STOPPED;
    }
    return rc;
}

ayni_status_t ayni_control_session_get_info(const ayni_control_session_t *session,
                                            ayni_control_session_info_v1 *out_info) {
    if (session == nullptr || out_info == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = structure(out_info->struct_size, sizeof(*out_info));
    if (shape != AYNI_OK) {
        return shape;
    }
    *out_info = {};
    out_info->struct_size = sizeof(*out_info);
    out_info->state = session->state;
    out_info->permit_id =
        session->state == AYNI_CONTROL_SESSION_IDLE ? 0U : session->permit.permit_id;
    out_info->segment_index = session->segment_index;
    out_info->segment_count = session->segment_count;
    out_info->last_sample_sequence = session->sequence;
    out_info->transitions = session->transitions;
    return AYNI_OK;
}
} // extern "C"
