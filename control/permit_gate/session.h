// Fixed-size streaming ControlSession state over ayni_control_gate (ADR-0112).
// No Core dependency, no heap allocation; caller arena owns this object until destroy.
#ifndef AYNI_CONTROL_PERMIT_GATE_SESSION_H
#define AYNI_CONTROL_PERMIT_GATE_SESSION_H
#include "gate.h"
#include <array>
#include <ayni/control_session.h>

// Caller-allocated session holding bounded segments and monotonic sample state.
struct ayni_control_session {
    ayni_control_gate_t *gate = nullptr; // Borrowed gate; null iff IDLE.
    ayni_execution_permit_v1 permit{};   // Copied normalized permit while attached.
    ayni_control_session_state_t state = AYNI_CONTROL_SESSION_IDLE; // Explicit lifecycle state.
    std::array<ayni_control_segment_v1, AYNI_CONTROL_SESSION_SEGMENTS_MAX> segments{};
    uint32_t segment_count = 0;        // Valid prefix count in segments[0..segment_count).
    uint32_t segment_index = 0;        // Active segment index when state == ACTIVE.
    uint64_t segment_started_us = 0;   // Executor-local monotonic start of active segment.
    uint64_t sequence = 0;             // Monotonic gate sample sequence within this permit.
    uint64_t transitions = 0;          // Count of seamless in-session segment switches.
    bool target_replaced = false;      // Set by replace_target(); cleared on next tick().
    bool first_sample_emitted = false; // False until tick() emits segment 0's first sample.

    // Validates one segment shape and field bounds against permit.envelope (reuses valid_sample).
    bool valid_segment(const ayni_control_segment_v1 &segment) const;
    // Emits one fresh sample for segments[segment_index] into the bound gate.
    ayni_status_t emit_sample(uint64_t now_us, const ayni_control_observation_v1 *live);
    // Populates out_step from current state without mutating gate or segments.
    void describe_step(uint64_t now_us, bool transitioned,
                       ayni_control_session_step_v1 &out_step) const;
};
#endif
