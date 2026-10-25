/* Local executor permit gate, Since 0.34. No Core dependency or hardware I/O.
 * All calls use one executor owner thread, outside interrupts. The trusted
 * adapter supplies executor-local monotonic time and measured interlocks;
 * permits arrive only through the authenticated authority-owner path. This
 * library is not a signature verifier or a process isolation boundary. The
 * trusted adapter must change executor_boot_epoch for each new gate lifetime;
 * recreating a gate with an old generation would permit old nonce/ID reuse.
 *
 * The caller disables outputs whenever a command/check is not OK, polls check
 * every control cycle, and confirms stop only from local measurement. Losing
 * the caller itself requires the independent hardware/watchdog stop chain.
 */
#ifndef AYNI_CONTROL_H
#define AYNI_CONTROL_H
#include <ayni/ayni.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum numeric fields in one execution envelope. Since 0.34. */
#define AYNI_CONTROL_FIELD_COUNT_MAX 8u
/* Executor retains exactly this many recent local nonce timestamps. Since 0.34. */
#define AYNI_CONTROL_NONCE_COUNT 8u

typedef uint32_t ayni_execution_form_t;
/* A bounded, nonrenewable procedure. Since 0.34. */
#define AYNI_FORM_PROCEDURE ((ayni_execution_form_t)1)
/* A session whose short lease must be renewed. Since 0.34. */
#define AYNI_FORM_SESSION ((ayni_execution_form_t)2)

/* All generations must match on every control cycle; no zero/default. Since 0.34. */
typedef struct ayni_permit_epochs_v1 {
    uint64_t kernel_boot_epoch;   /* Current authority Kernel boot, nonzero. */
    uint64_t authority_epoch;     /* Current fixed owner binding generation, nonzero. */
    uint64_t executor_boot_epoch; /* Fresh executor/gate generation, nonzero; changes on every gate
                                     creation. */
    uint64_t safety_epoch;        /* Local stop/reset generation, nonzero. */
    uint32_t calibration_version; /* Enabled local calibration version, nonzero. */
    uint32_t reserved;            /* Zero. */
} ayni_permit_epochs_v1;

/* Numeric domain bounds, not a serialized hash or an implicit unit conversion. Since 0.34. */
typedef struct ayni_control_range_v1 {
    ayni_catalog_name_v1 field_id; /* Nonempty catalog ID; unused tail bytes ignored. */
    ayni_catalog_name_v1 unit;     /* Nonempty exact unit ID; no implicit conversion. */
    int64_t minimum;               /* Inclusive lower bound in unit; <= maximum. */
    int64_t maximum;               /* Inclusive upper bound in unit. */
} ayni_control_range_v1;

/* Full copied range set; field order has no meaning, duplicate IDs rejected. Since 0.34. */
typedef struct ayni_control_envelope_v1 {
    uint32_t count;    /* 1..FIELD_COUNT_MAX; every configured channel is required. */
    uint32_t reserved; /* Zero; cells outside count are ignored. */
    ayni_control_range_v1 fields[AYNI_CONTROL_FIELD_COUNT_MAX]; /* Valid prefix only. */
} ayni_control_envelope_v1;

/* Trusted startup limits for one executor/control mode. Copies all input. Since 0.34. */
typedef struct ayni_control_config_v1 {
    uint32_t struct_size;         /* Exact sizeof. */
    uint32_t abi_major;           /* Exact AYNI_ABI_MAJOR, including standalone gate deployments. */
    uint32_t abi_minor;           /* Exact AYNI_ABI_MINOR while major is zero. */
    uint32_t control_mode;        /* Nonzero locally defined mode. */
    uint64_t control_domain_id;   /* Nonzero fixed local domain. */
    ayni_permit_epochs_v1 epochs; /* Expected owner/executor generations. */
    uint32_t procedure_max_us;    /* Positive maximum nonrenewable duration, microseconds. */
    uint32_t session_lease_max_us;   /* Positive maximum renewable lease, microseconds. */
    uint32_t command_age_max_us;     /* Positive maximum local sample age, microseconds. */
    uint32_t reserved;               /* Zero. */
    ayni_control_envelope_v1 limits; /* Measured device limits; cannot be enlarged by permits. */
} ayni_control_config_v1;

/* Shared in-process permit value, not a device wire format. Since 0.34. */
typedef struct ayni_execution_permit_v1 {
    uint32_t struct_size;         /* Exact sizeof. */
    ayni_execution_form_t form;   /* PROCEDURE or SESSION. */
    uint64_t permit_id;           /* Positive, strictly increasing per configured Kernel boot. */
    uint64_t operation_id;        /* Stable action identity; never inferred from permit_id. */
    uint64_t control_domain_id;   /* Exact configured executor domain. */
    ayni_permit_epochs_v1 epochs; /* Must equal trusted startup and live observation. */
    uint64_t echoed_nonce;        /* Executor-issued nonce retained in its local window. */
    uint32_t control_mode;        /* Exact configured mode. */
    uint32_t lease_us; /* Positive L from capability; issuer includes drift margin; anchored at
                          nonce issue. */
    uint32_t max_command_age_us;       /* Positive local sample-age bound, <= device bound. */
    uint32_t reserved;                 /* Zero. */
    ayni_control_envelope_v1 envelope; /* Complete approved bounds, subset of device limits. */
} ayni_execution_permit_v1;

/* Local executor freshness challenge; timestamp is never supplied by issuer. Since 0.34. */
typedef struct ayni_executor_nonce_v1 {
    uint64_t control_domain_id; /* Configured domain, nonzero. */
    uint64_t nonce;             /* Positive monotonic nonce, never reused by this gate. */
} ayni_executor_nonce_v1;

/* Renewal changes only the deadline, never the approved execution scope. Since 0.34. */
typedef struct ayni_permit_renewal_v1 {
    uint32_t struct_size;         /* Exact sizeof. */
    uint32_t reserved;            /* Zero. */
    uint64_t permit_id;           /* Exact live SESSION; no terminated permit may renew. */
    uint64_t echoed_nonce;        /* Retained executor challenge. */
    ayni_permit_epochs_v1 epochs; /* Exact original generations. */
} ayni_permit_renewal_v1;

/* Supplied freshly by a trusted local adapter on each evaluation. Since 0.34. */
typedef struct ayni_control_observation_v1 {
    ayni_permit_epochs_v1
        epochs;             /* Current measured/configured generations, never cached remotely. */
    uint32_t interlocks_ok; /* Exactly 1 iff all declared local interlocks permit output. */
    uint32_t reserved;      /* Zero. */
} ayni_control_observation_v1;

/* One local sample; all fields of the accepted envelope must be present. Since 0.34. */
typedef struct ayni_control_value_v1 {
    ayni_catalog_name_v1 field_id; /* Exact declared field. */
    ayni_catalog_name_v1 unit;     /* Exact declared unit. */
    int64_t value;                 /* In inclusive accepted bounds. */
} ayni_control_value_v1;

/* Sample times are in the executor clock; a remote arrival is not capture time. Since 0.34. */
typedef struct ayni_control_sample_v1 {
    uint32_t struct_size;   /* Exact sizeof. */
    uint32_t count;         /* Exactly the accepted envelope count. */
    uint64_t permit_id;     /* Exact active permit. */
    uint64_t sequence;      /* Positive, strictly increasing within this permit. */
    uint64_t sampled_at_us; /* Trusted local capture time; may be zero, never future. */
    ayni_control_value_v1 fields[AYNI_CONTROL_FIELD_COUNT_MAX]; /* Complete unique field set. */
} ayni_control_sample_v1;

typedef uint32_t ayni_control_phase_t;
/* No permit has been accepted, output disabled. Since 0.34. */
#define AYNI_CONTROL_IDLE ((ayni_control_phase_t)1)
/* Permit is active, but each sample still needs evaluation. Since 0.34. */
#define AYNI_CONTROL_ACTIVE ((ayni_control_phase_t)2)
/* Output forbidden; local stop measurement still outstanding. Since 0.34. */
#define AYNI_CONTROL_STOPPING ((ayni_control_phase_t)3)
/* Stop measured locally; old permit is permanently terminal. Since 0.34. */
#define AYNI_CONTROL_STOPPED ((ayni_control_phase_t)4)

typedef uint32_t ayni_control_stop_cause_t;
/* No stop requested for this permit. Since 0.34. */
#define AYNI_CONTROL_STOP_NONE ((ayni_control_stop_cause_t)0)
/* Trusted local cancellation/normal execution end. Since 0.34. */
#define AYNI_CONTROL_STOP_REQUESTED ((ayni_control_stop_cause_t)1)
/* Nonce-anchored lease deadline reached. Since 0.34. */
#define AYNI_CONTROL_STOP_EXPIRED ((ayni_control_stop_cause_t)2)
/* A current generation differs from the permit. Since 0.34. */
#define AYNI_CONTROL_STOP_EPOCH ((ayni_control_stop_cause_t)3)
/* A declared local interlock no longer permits output. Since 0.34. */
#define AYNI_CONTROL_STOP_INTERLOCK ((ayni_control_stop_cause_t)4)
/* A sample is malformed, stale, repeated or outside its approved envelope. Since 0.34. */
#define AYNI_CONTROL_STOP_SAMPLE ((ayni_control_stop_cause_t)5)
/* The executor clock moved backwards; recreate after measured stop. Since 0.34. */
#define AYNI_CONTROL_STOP_CLOCK ((ayni_control_stop_cause_t)6)

/* Copied execution and stop facts; never a target-achievement claim. Since 0.34. */
typedef struct ayni_control_info_v1 {
    uint32_t struct_size;                 /* Caller supplies exact sizeof. */
    ayni_control_phase_t phase;           /* Explicit lifecycle; IDLE has zero identities. */
    uint64_t permit_id;                   /* Last accepted permit, zero only for IDLE. */
    uint64_t operation_id;                /* Last accepted operation, zero only for IDLE. */
    uint64_t deadline_us;                 /* Executor-local deadline; zero only for IDLE. */
    uint64_t last_sample_sequence;        /* Zero before an accepted sample of this permit. */
    uint64_t stop_confirmed_at_us;        /* Local measurement time, meaningful only in STOPPED. */
    ayni_control_stop_cause_t stop_cause; /* First reason for stopping; NONE for IDLE/ACTIVE. */
    uint32_t reserved;                    /* Written zero. */
} ayni_control_info_v1;

/* Opaque gate; all memory belongs to caller, no heap allocation. Since 0.34. */
typedef struct ayni_control_gate ayni_control_gate_t;
/* OK or INVALID_ARGUMENT for null outputs; fixed bounded storage. Since 0.34. */
ayni_status_t ayni_control_memory_requirements(size_t *out_size, size_t *out_alignment);
/* OK, INVALID_ARGUMENT, UNSUPPORTED_VERSION, BUFFER_TOO_SMALL. Failures preserve
 * outputs/storage. Storage is aligned, exclusive and retained until destroy. Since 0.34. */
ayni_status_t ayni_control_create(const ayni_control_config_v1 *config, uint64_t now_us,
                                  void *storage, size_t storage_size,
                                  ayni_control_gate_t **out_gate);
/* OK, INVALID_ARGUMENT; BUSY for ACTIVE/STOPPING. No implicit stop. Since 0.34. */
ayni_status_t ayni_control_destroy(ayni_control_gate_t *gate);
/* Issues a local challenge; OK, INVALID_ARGUMENT, CAPACITY_EXCEEDED, STALE
 * (clock regression). May expire an active lease before returning. Since 0.34. */
ayni_status_t ayni_control_nonce(ayni_control_gate_t *gate, uint64_t now_us,
                                 ayni_executor_nonce_v1 *out_nonce);
/* After required pointer checks, start/renew/check/sample observe time and live interlocks. Failure
 * may stop an active permit; output is never authorized by an error status.
 * OK, INVALID_ARGUMENT, UNSUPPORTED_VERSION, STALE, EXPIRED, DENIED, BUSY,
 * INVALID_STATE, NOT_READY as applicable; no automatic restart. Since 0.34. */
ayni_status_t ayni_control_start(ayni_control_gate_t *gate, const ayni_execution_permit_v1 *permit,
                                 uint64_t now_us, const ayni_control_observation_v1 *live);
/* Only active SESSION; no lease/envelope changes. Since 0.34. */
ayni_status_t ayni_control_renew(ayni_control_gate_t *gate, const ayni_permit_renewal_v1 *renewal,
                                 uint64_t now_us, const ayni_control_observation_v1 *live);
/* Every local control cycle, even without a new sample. NOT_READY until the first
 * accepted sample; stale last samples stop the permit even within its lease. Since 0.34. */
ayni_status_t ayni_control_check(ayni_control_gate_t *gate, uint64_t now_us,
                                 const ayni_control_observation_v1 *live);
/* Validates identity, sequence, age and all numeric fields; no clamping. Since 0.34. */
ayni_status_t ayni_control_sample(ayni_control_gate_t *gate, const ayni_control_sample_v1 *sample,
                                  uint64_t now_us, const ayni_control_observation_v1 *live);
/* Requests stop independently of queues. OK, INVALID_ARGUMENT, STALE or
 * INVALID_STATE for wrong/absent permit; repeated exact stop is OK. Since 0.34. */
ayni_status_t ayni_control_stop(ayni_control_gate_t *gate, uint64_t permit_id, uint64_t now_us);
/* Trusted local measurement only, never cancellation acceptance or inference.
 * OK, INVALID_ARGUMENT, STALE or INVALID_STATE unless exact STOPPING/STOPPED;
 * repeated confirmation keeps the original measurement time. After a clock
 * regression, a fresh local measurement still allows cleanup; the clock fault
 * continues to forbid admission until recreation with a new generation. Since 0.34. */
ayni_status_t ayni_control_confirm_stopped(ayni_control_gate_t *gate, uint64_t permit_id,
                                           uint64_t measured_at_us);
/* OK, INVALID_ARGUMENT, UNSUPPORTED_VERSION; pure copy, does not tick time. Since 0.34. */
ayni_status_t ayni_control_get_info(const ayni_control_gate_t *gate,
                                    ayni_control_info_v1 *out_info);

#ifdef __cplusplus
}
#endif
#endif
