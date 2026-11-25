/* Bounded local output shaping, not a certified safety function or independent stop chain.
 * No Core dependency, I/O, allocation or hidden state. Since 0.61. */
#ifndef AYNI_CONTROL_LOOP_H
#define AYNI_CONTROL_LOOP_H
#include <ayni/control_session.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum independently timed integer sensor channels. Since 0.61. */
#define AYNI_CONTROL_LOOP_SENSORS_MAX 4u
/* Percentage denominator; hold=100 means no additional hold reduction. Since 0.61. */
#define AYNI_CONTROL_LOOP_FULL_SCALE 100u

typedef uint32_t ayni_control_loop_action_t;
/* Effective values may be applied only through PermitGate. Since 0.61. */
#define AYNI_LOOP_APPLY ((ayni_control_loop_action_t)1)
/* Caller must stop hardware and confirm output disable; no output authorized. Since 0.61. */
#define AYNI_LOOP_SOFT_STOP ((ayni_control_loop_action_t)2)

/* One required sensor with an increasing-safe integer measurement. Since 0.61. */
typedef struct ayni_control_sensor_limit_v1 {
    uint32_t source_id;  /* Positive host-local identity; unique in config. */
    uint32_t reserved;   /* Zero. */
    uint64_t max_age_us; /* Positive inclusive capture age in executor microseconds. */
    int64_t stop_value;  /* Hard-stop at or below this measurement, in sensor units. */
    int64_t slow_value;  /* Full speed above this measurement; strictly greater than stop_value. */
} ayni_control_sensor_limit_v1;
/* Copied measured observation; absent or degraded required channels cause soft stop. Since 0.61. */
typedef struct ayni_control_sensor_sample_v1 {
    uint32_t source_id;      /* Positive matching configured sensor. */
    uint32_t measured_ok;    /* Exactly 1 for fresh MEASURED/non-DEGRADED data; 0 is unavailable. */
    uint64_t captured_at_us; /* Executor clock microseconds; future captures are invalid. */
    int64_t value;           /* Integer measurement in the configured sensor's units. */
} ayni_control_sensor_sample_v1;
/* Immutable limiter configuration and explicit prior-cycle state. Since 0.61. */
typedef struct ayni_control_loop {
    uint32_t struct_size; /* Exact sizeof; caller initializes complete value with zero. */
    uint32_t field_count; /* 1..CONTROL_FIELD_COUNT_MAX unique output fields to scale. */
    ayni_catalog_name_v1 fields[AYNI_CONTROL_FIELD_COUNT_MAX]; /* Exact desired IDs; tail zero. */
    uint32_t sensor_count; /* 0..SENSORS_MAX required sensors; 0 disables sensor shaping. */
    uint32_t has_previous; /* 0 at session start, 1 with a recorded prior effective/last_step. */
    ayni_control_sensor_limit_v1 sensors[AYNI_CONTROL_LOOP_SENSORS_MAX]; /* Active prefix. */
    uint64_t max_cycle_gap_us; /* Positive inclusive allowed gap; regression also soft stops. */
    uint64_t slew_per_step;    /* Maximum magnitude INCREASE in field units per cycle; 0 disables.
                                * Reductions bypass slew so braking is never delayed. */
    uint64_t last_step_us;     /* Previous executor capture time iff has_previous=1, otherwise 0. */
    ayni_control_segment_v1 previous; /* Prior effective setpoint iff has_previous=1. */
} ayni_control_loop_t;

/* Pure deterministic transition: out_next receives explicit next state, input loop is unchanged.
 * Caller adopts out_next each cycle, including SOFT_STOP; aliasing loop/out_next is allowed.
 * Sensor/hold scale only narrows listed fields; other desired fields/duration stay exact.
 * Missing, stale, degraded, future sensor, cycle overrun/regression => OK + SOFT_STOP with zero
 * controlled outputs. Structural errors return status and also initialize action=SOFT_STOP.
 * out_effective and out_next must not overlap each other or desired. Since 0.61. */
ayni_status_t
ayni_control_loop_limit(const ayni_control_loop_t *loop, const ayni_control_segment_v1 *desired,
                        const ayni_control_sensor_sample_v1 *sensors, uint32_t sensor_count,
                        uint64_t now_us, uint32_t hold_scale_percent,
                        ayni_control_segment_v1 *out_effective,
                        ayni_control_loop_action_t *out_action, ayni_control_loop_t *out_next);

#ifdef __cplusplus
}
#endif
#endif
