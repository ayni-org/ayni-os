/* Streaming multi-segment ControlSession over ayni_control_gate, Since 0.59 (ADR-0112).
 * Zero Core dependency, zero heap allocation, single executor owner thread outside ISRs.
 * Enforces ayni_control_sample() every control cycle across segment handoffs and renewals.
 */
#ifndef AYNI_CONTROL_SESSION_H
#define AYNI_CONTROL_SESSION_H
#include <ayni/control.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum buffered trajectory segments in one bounded ControlSession. Since 0.59. */
#define AYNI_CONTROL_SESSION_SEGMENTS_MAX 8u

typedef uint32_t ayni_control_session_state_t;
/* No active permit attached; output disabled. Since 0.59. */
#define AYNI_CONTROL_SESSION_IDLE ((ayni_control_session_state_t)1)
/* Streaming active permit; caller outputs step.active while tick returns OK. Since 0.59. */
#define AYNI_CONTROL_SESSION_ACTIVE ((ayni_control_session_state_t)2)
/* All queued segments completed normally; caller must stop hardware and confirm. Since 0.59. */
#define AYNI_CONTROL_SESSION_COMPLETED ((ayni_control_session_state_t)3)
/* Stop requested or gate faulted; output forbidden until confirm_stopped(). Since 0.59. */
#define AYNI_CONTROL_SESSION_STOPPING ((ayni_control_session_state_t)4)
/* Local stop confirmed; permit is permanently terminal and session may reopen. Since 0.59. */
#define AYNI_CONTROL_SESSION_STOPPED ((ayni_control_session_state_t)5)

/* One bounded setpoint segment within an authorized permit envelope. Since 0.59. */
typedef struct ayni_control_segment_v1 {
    uint32_t struct_size; /* Exact sizeof. */
    uint32_t field_count; /* Must equal permit.envelope.count. */
    uint64_t
        duration_us; /* Positive segment duration, or 0 for continuous until replaced/stopped. */
    ayni_control_value_v1 fields[AYNI_CONTROL_FIELD_COUNT_MAX]; /* Complete unique field set. */
} ayni_control_segment_v1;

/* Per-cycle output of ayni_control_session_tick / advance_segment. Since 0.59. */
typedef struct ayni_control_session_step_v1 {
    uint32_t struct_size;               /* Caller supplies exact sizeof. */
    ayni_control_session_state_t state; /* Current session state after evaluation. */
    uint32_t segment_index;             /* Zero-based index of active segment in trajectory. */
    uint32_t segment_count;             /* Total segments currently loaded in trajectory. */
    uint32_t transitioned;              /* 1 iff active segment changed on this call, else 0. */
    uint32_t reserved;                  /* Written zero. */
    uint64_t remaining_us;              /* Remaining segment duration; 0 for unbounded/inactive. */
    ayni_control_segment_v1 active;     /* Current validated setpoint when state == ACTIVE. */
} ayni_control_session_step_v1;

/* Copied read-only session snapshot. Since 0.59. */
typedef struct ayni_control_session_info_v1 {
    uint32_t struct_size;               /* Caller supplies exact sizeof. */
    ayni_control_session_state_t state; /* Explicit session lifecycle. */
    uint64_t permit_id;                 /* Attached permit ID, or 0 when IDLE. */
    uint32_t segment_index;             /* Current segment index. */
    uint32_t segment_count;             /* Loaded segment count. */
    uint64_t last_sample_sequence;      /* Last sequence accepted by gate. */
    uint64_t transitions;               /* Seamless in-session segment switches completed. */
} ayni_control_session_info_v1;

/* Opaque caller-owned session; fixed bounded storage, no heap allocation. Since 0.59. */
typedef struct ayni_control_session ayni_control_session_t;

/* Fixed storage requirements for placement construction. Since 0.59. */
ayni_status_t ayni_control_session_memory_requirements(size_t *out_size, size_t *out_alignment);
/* Construct an IDLE session in caller-aligned memory. Since 0.59. */
ayni_status_t ayni_control_session_create(void *storage, size_t storage_size,
                                          ayni_control_session_t **out_session);
/* Destroy an IDLE or STOPPED session; returns BUSY while ACTIVE/COMPLETED/STOPPING. Since 0.59. */
ayni_status_t ayni_control_session_destroy(ayni_control_session_t *session);
/* Open a permit on gate and validate all initial segments[0..segment_count) against its envelope.
 * Rejects without starting the gate if any segment is malformed or outside permit.envelope.
 * Since 0.59. */
ayni_status_t ayni_control_session_open(ayni_control_session_t *session, ayni_control_gate_t *gate,
                                        const ayni_execution_permit_v1 *permit,
                                        const ayni_control_segment_v1 *segments,
                                        uint32_t segment_count, uint64_t now_us,
                                        const ayni_control_observation_v1 *live);
/* Append one segment to an ACTIVE AYNI_FORM_SESSION without re-handshaking; validates against
 * the active permit envelope. Returns CAPACITY_EXCEEDED if full, INVALID_STATE unless ACTIVE
 * SESSION, DENIED if outside envelope. Since 0.59. */
ayni_status_t ayni_control_session_push_segment(ayni_control_session_t *session,
                                                const ayni_control_segment_v1 *segment);
/* Replace the active setpoint and clear pending segments on an ACTIVE AYNI_FORM_SESSION
 * (for closed-loop / joystick streaming inside the approved envelope). Since 0.59. */
ayni_status_t ayni_control_session_replace_target(ayni_control_session_t *session,
                                                  const ayni_control_segment_v1 *target,
                                                  uint64_t now_us);
/* Forward a short-lease renewal to the bound gate without interrupting output. Since 0.59. */
ayni_status_t ayni_control_session_renew(ayni_control_session_t *session,
                                         const ayni_permit_renewal_v1 *renewal, uint64_t now_us,
                                         const ayni_control_observation_v1 *live);
/* Evaluate time, interlocks, segment duration expiry, and submit a fresh gate sample.
 * Switches segments atomically within the same tick without stopping the gate. Since 0.59. */
ayni_status_t ayni_control_session_tick(ayni_control_session_t *session, uint64_t now_us,
                                        const ayni_control_observation_v1 *live,
                                        ayni_control_session_step_v1 *out_step);
/* Hardware-timed handoff: advance immediately to the next queued segment on exact hardware
 * completion, validating it via ayni_control_sample() in the same call. Moves to COMPLETED
 * when no next segment remains. Since 0.59. */
ayni_status_t ayni_control_session_advance_segment(ayni_control_session_t *session, uint64_t now_us,
                                                   const ayni_control_observation_v1 *live,
                                                   ayni_control_session_step_v1 *out_step);
/* Request recoverable stop on the attached gate and enter STOPPING. Since 0.59. */
ayni_status_t ayni_control_session_soft_stop(ayni_control_session_t *session, uint64_t now_us);
/* Confirm measured local output stop on the attached gate and enter STOPPED. Since 0.59. */
ayni_status_t ayni_control_session_confirm_stopped(ayni_control_session_t *session,
                                                   uint64_t measured_at_us);
/* Pure diagnostic snapshot; does not tick time. Since 0.59. */
ayni_status_t ayni_control_session_get_info(const ayni_control_session_t *session,
                                            ayni_control_session_info_v1 *out_info);

#ifdef __cplusplus
}
#endif
#endif
