/* Local deterministic sensor-to-action reflex rules, Since 0.59 (ADR-0112).
 * Rules register in CONFIGURING, evaluate synchronously on the Kernel owner
 * thread right after draining lock-free ObservationSlots (0 LLM, 0 WAL), and
 * never bypass Grant, Scope, Scene, or PermitGate boundaries.
 */
#ifndef AYNI_REFLEX_H
#define AYNI_REFLEX_H
#include <ayni/observations.h>
#include <ayni/values.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum startup ReflexRules; zero configured capacity disables them. Since 0.59. */
#define AYNI_REFLEX_CAPACITY_MAX 32u
/* Default bounded event ring entries when reflex_capacity > 0. Since 0.59. */
#define AYNI_REFLEX_EVENT_CAPACITY_DEFAULT 16u
/* Maximum bounded event ring entries. Since 0.59. */
#define AYNI_REFLEX_EVENT_CAPACITY_MAX 64u
/* Maximum observation conditions per ReflexRule. Since 0.59. */
#define AYNI_REFLEX_CONDITION_COUNT_MAX 4u

typedef uint32_t ayni_reflex_action_t;
/* Soft-stop and preempt cancellable held EPHEMERAL Calls on resource_id. Since 0.59. */
#define AYNI_REFLEX_SOFT_STOP_RESOURCE ((ayni_reflex_action_t)1)
/* Preempt via a pre-registered deterministic EPHEMERAL Call after Grant/Scene preflight.
 * Since 0.59. */
#define AYNI_REFLEX_PREEMPT_CALL ((ayni_reflex_action_t)2)

typedef uint32_t ayni_reflex_trigger_mode_t;
/* Fires when all conditions are MEASURED, GOOD, unexpired, and within [min, max]. Since 0.59. */
#define AYNI_REFLEX_TRIGGER_IN_RANGE ((ayni_reflex_trigger_mode_t)1)
/* Fail-safe: fires SOFT_STOP_RESOURCE if any listed source is absent, expired, DEGRADED,
 * or invalidated by a gap while an EPHEMERAL Call holds resource_id. Since 0.59. */
#define AYNI_REFLEX_TRIGGER_STALE_OR_GAP ((ayni_reflex_trigger_mode_t)2)

/* Rule starts enabled after recovery without an extra owner call. Since 0.59. */
#define AYNI_REFLEX_FLAG_ENABLED_BY_DEFAULT 0x1u
/* Fire once on false->true edge; re-arm only after condition clears. Since 0.59. */
#define AYNI_REFLEX_FLAG_EDGE_TRIGGERED 0x2u

/* Maximum dynamic integer bindings and object-only path depth per rule. Since 0.61. */
#define AYNI_REFLEX_PARAM_BINDING_MAX 4u
#define AYNI_REFLEX_PARAM_PATH_MAX 4u
/* Deterministic map: clamp input; saturated (input*scale_num)/scale_den truncated toward zero;
 * saturated offset addition; clamp output. Only MEASURED condition sources and fixed I64
 * schema fields are supported. No floating point or list paths. Since 0.61. */
typedef struct ayni_reflex_param_binding_v1 {
    uint32_t condition_index; /* Index in the same rule's active conditions prefix. */
    uint32_t path_count;      /* 1..PARAM_PATH_MAX positive object field IDs. */
    uint32_t path[AYNI_REFLEX_PARAM_PATH_MAX]; /* Root-to-leaf path; unused tail zero. */
    int64_t input_min;  /* Inclusive measurement clamp lower bound in source units. */
    int64_t input_max;  /* Inclusive measurement clamp upper bound, >= input_min. */
    int64_t scale_num;  /* Signed integer affine slope numerator. */
    int64_t scale_den;  /* Positive denominator, never zero. */
    int64_t offset;     /* Saturating addition after division, in output field units. */
    int64_t output_min; /* Inclusive output clamp lower bound within the entire schema range. */
    int64_t output_max; /* Inclusive output clamp upper bound, >= output_min. */
} ayni_reflex_param_binding_v1;

/* Trusted CONFIGURING registration; all views and value inputs are copied. Since 0.59. */
typedef struct ayni_reflex_rule_v1 {
    uint32_t struct_size;                    /* Exact sizeof. */
    uint32_t version;                        /* Positive semantic rule version. */
    ayni_string_v1 rule_id;                  /* Unique nonempty catalog identity. */
    ayni_reflex_action_t action;             /* SOFT_STOP_RESOURCE or PREEMPT_CALL. */
    ayni_reflex_trigger_mode_t trigger_mode; /* IN_RANGE or STALE_OR_GAP (SOFT_STOP only). */
    uint32_t flags;                          /* Zero or AYNI_REFLEX_FLAG_* bits. */
    uint32_t priority;                       /* 0..255; higher evaluated first. */
    uint64_t cooldown_us;                    /* Minimum interval between consecutive fires. */
    ayni_string_v1 scene_id;                 /* Optional Scene gate; empty disables Scene gate. */
    uint32_t scene_version;   /* Exact Scene version iff scene_id nonempty, else 0. */
    uint32_t condition_count; /* 1..AYNI_REFLEX_CONDITION_COUNT_MAX unique sources. */
    const ayni_observation_condition_v1 *conditions; /* MEASURED sources only; copied. */
    ayni_string_v1 resource_id; /* Required for SOFT_STOP_RESOURCE; empty for PREEMPT_CALL. */
    uint64_t owner_agent_id;    /* Required positive agent ID for PREEMPT_CALL; else 0. */
    ayni_string_v1 binding_id;  /* Required EPHEMERAL binding for PREEMPT_CALL; else empty. */
    uint32_t binding_version;   /* Positive exact binding version for PREEMPT_CALL; else 0. */
    uint32_t reserved;          /* Zero. */
    ayni_value_v1 input; /* Canonical business value for PREEMPT_CALL; empty for SOFT_STOP. */
    uint32_t param_binding_count;    /* 0..PARAM_BINDING_MAX; zero disables mapping. Since 0.61. */
    uint32_t param_binding_reserved; /* Zero. Since 0.61. */
    const ayni_reflex_param_binding_v1 *param_bindings; /* Copied at registration; null iff zero.
                                                         * PREEMPT_CALL only. Since 0.61. */
} ayni_reflex_rule_v1;

/* Owner-thread diagnostics for one registered ReflexRule. Since 0.59. */
typedef struct ayni_reflex_info_v1 {
    uint32_t struct_size;            /* Caller supplies exact sizeof. */
    uint32_t version;                /* Registered rule version. */
    ayni_reflex_action_t action;     /* Registered action kind. */
    ayni_reflex_trigger_mode_t mode; /* Registered trigger mode. */
    uint32_t enabled;                /* 1 iff currently enabled in RAM. */
    uint32_t latched_active;         /* 1 iff edge-triggered rule currently awaits reset. */
    uint64_t last_fired_us;          /* Monotonic time of last fire; 0 if never fired. */
    uint64_t fire_count;             /* Total accepted/attempted fires in current boot. */
    uint64_t denied_count;           /* Triggers refused by Grant/Scene/capacity preflight. */
    uint64_t active_call_id;         /* Live reflex Call ID for PREEMPT_CALL, or 0. */
} ayni_reflex_info_v1;

/* One asynchronous reflex notification drained by Host/UI/upper layers. Since 0.59. */
typedef struct ayni_reflex_event_v1 {
    uint32_t struct_size;                /* Caller supplies exact sizeof. */
    uint32_t reserved;                   /* Written zero. */
    uint64_t sequence;                   /* Positive monotonic event number in this boot. */
    uint64_t fired_at_us;                /* Owner monotonic timestamp when rule fired. */
    ayni_reflex_action_t action;         /* Rule action kind. */
    ayni_status_t status;                /* OK on applied stop/dispatch; error on refusal. */
    uint64_t affected_call_id;           /* Preempted/soft-stopped holder Call ID, or 0. */
    uint64_t spawned_call_id;            /* Reflex EPHEMERAL Call ID for PREEMPT_CALL, or 0. */
    int64_t trigger_value;               /* Observed scalar value (0 for absent/stale source). */
    uint64_t dropped_before;             /* Total older events overwritten due to ring bound. */
    ayni_catalog_name_v1 rule_id;        /* Fired rule identity. */
    ayni_catalog_name_v1 trigger_source; /* First matching or stale source ID. */
} ayni_reflex_event_v1;

/* CONFIGURING owner-thread registration; sources/bindings/scenes resolved at ayni_start().
 * Standard argument/version/state/capacity errors. Since 0.59. */
ayni_status_t ayni_register_reflex_rule(ayni_kernel_t *kernel, const ayni_reflex_rule_v1 *rule);
/* RUNNING owner-thread volatile RAM toggle (0 WAL). enabled must be 0 or 1. Since 0.59. */
ayni_status_t ayni_set_reflex_enabled(ayni_kernel_t *kernel, ayni_string_v1 rule_id,
                                      uint32_t enabled);
/* Owner-thread diagnostic copy; NOT_FOUND if rule_id is unregistered. Since 0.59. */
ayni_status_t ayni_get_reflex_info(const ayni_kernel_t *kernel, ayni_string_v1 rule_id,
                                   ayni_reflex_info_v1 *out_info);
/* Owner-thread nonblocking drain of oldest reflex event; NOT_FOUND when ring is empty.
 * Since 0.59. */
ayni_status_t ayni_take_reflex_event(ayni_kernel_t *kernel, ayni_reflex_event_v1 *out_event);

#ifdef __cplusplus
}
#endif
#endif
