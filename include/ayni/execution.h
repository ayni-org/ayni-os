/* Managed local Mock execution ports and binding constraints, Since 0.35.
 * All ports run on the Kernel owner thread, are bounded/nonblocking and must
 * not reenter Core. Contexts survive successful Kernel destruction. Executor
 * command/sample loops remain outside Core and enforce control.h every cycle.
 */
#ifndef AYNI_EXECUTION_H
#define AYNI_EXECUTION_H
#include <ayni/control.h>
#include <ayni/observations.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum registered local executors; zero capacity disables. Since 0.35. */
#define AYNI_EXECUTOR_CAPACITY_MAX 32u
/* Maximum exact binding constraints; zero capacity disables. Since 0.35. */
#define AYNI_EXECUTION_BINDING_CAPACITY_MAX 64u

typedef uint32_t ayni_execution_target_t;
/* No measurement has established the requested target. Since 0.35. */
#define AYNI_TARGET_UNVERIFIED ((ayni_execution_target_t)0)
/* Trusted local measurement confirms the target. Since 0.35. */
#define AYNI_TARGET_SATISFIED ((ayni_execution_target_t)1)
/* Trusted local measurement disproves the target. Since 0.35. */
#define AYNI_TARGET_UNSATISFIED ((ayni_execution_target_t)2)

/* Executor's current measured facts for one exact permit. Since 0.35. */
typedef struct ayni_executor_status_v1 {
    uint32_t struct_size;           /* Exact sizeof. */
    ayni_execution_target_t target; /* Independent of execution end/physical stop. */
    ayni_control_info_v1 control; /* Fresh local gate snapshot after evaluating time/interlocks. */
} ayni_executor_status_v1;

/* Trusted registration, copied except the runtime-owned callback context. Since 0.35.
 * M1 local Mock only: callbacks exchange typed in-process facts, never network bytes.
 * challenge returns a freshly issued local nonce, never an issuer timestamp.
 * status returns NOT_FOUND only if this gate never accepted the exact permit;
 * it is authoritative local evidence, unlike external Provider query absence.
 */
typedef struct ayni_executor_v1 {
    uint32_t struct_size;          /* Exact sizeof. */
    uint32_t version;              /* Positive exact implementation semantics version. */
    ayni_string_v1 executor_id;    /* Copied nonempty identity. */
    uint64_t control_domain_id;    /* Positive fixed domain, unique within this Kernel. */
    uint64_t executor_boot_epoch;  /* Fresh for every local gate lifetime, positive. */
    uint64_t safety_epoch;         /* Positive independent stop/reset generation. */
    uint32_t calibration_version;  /* Positive fixed calibration generation. */
    uint32_t control_mode;         /* Positive fixed local mode. */
    uint32_t procedure_max_us;     /* Positive finite duration bound, microseconds. */
    uint32_t session_lease_max_us; /* Positive lease duration bound, microseconds. */
    uint32_t command_age_max_us;   /* Positive local sample-age bound, microseconds. */
    uint32_t poll_period_us;       /* Positive trusted Core polling interval, <= shortest lease. */
    void *ctx;                     /* Runtime-owned until Kernel is successfully destroyed. */
    ayni_status_t (*challenge)(
        void *ctx, const ayni_permit_epochs_v1 *expected,
        ayni_executor_nonce_v1 *out_nonce); /* Required, copied output on OK. */
    ayni_status_t (*renew)(
        void *ctx,
        const ayni_permit_renewal_v1 *renewal); /* Required, local gate validates expiry. */
    ayni_status_t (*stop)(
        void *ctx, uint64_t permit_id); /* Required independent stop request, not confirmation. */
    ayni_status_t (*status)(
        void *ctx, uint64_t permit_id,
        ayni_executor_status_v1 *out_status); /* Required, fresh exact local facts. */
} ayni_executor_v1;

/* The same canonical authorized request supplies both numeric endpoints.
 * Use the same field twice for a single permitted value. Since 0.35. */
typedef struct ayni_execution_field_v1 {
    ayni_string_v1 field_id;   /* Copied control channel name. */
    ayni_string_v1 unit;       /* Copied exact unit, equal to both authority fields. */
    uint32_t minimum_field_id; /* Positive numeric Capability field ID. */
    uint32_t maximum_field_id; /* Positive numeric Capability field ID. */
} ayni_execution_field_v1;

/* Trusted finite requirements for one existing physical action Binding. Since 0.35. */
typedef struct ayni_execution_binding_v1 {
    uint32_t struct_size;        /* Exact sizeof. */
    uint32_t binding_version;    /* Positive exact registered Binding version. */
    ayni_string_v1 binding_id;   /* Copied existing binding identity. */
    ayni_string_v1 executor_id;  /* Copied exact executor selection. */
    uint32_t executor_version;   /* Positive exact semantics version. */
    uint32_t scene_version;      /* Positive exact Scene version, explicitly enabled by owner. */
    ayni_string_v1 scene_id;     /* Copied required Scene identity. */
    ayni_execution_form_t form;  /* PROCEDURE or SESSION. */
    uint32_t lease_us;           /* Positive finite lease, <= matching executor bound. */
    uint32_t max_command_age_us; /* Positive finite sample bound, <= executor bound. */
    uint32_t field_count;        /* 1..CONTROL_FIELD_COUNT_MAX unique channels. */
    const ayni_execution_field_v1 *fields; /* Copied complete mappings, never model-selected. */
} ayni_execution_binding_v1;

typedef uint32_t ayni_execution_phase_t;
/* Admitted identity/constraints, no permit has been dispatched. Since 0.35. */
#define AYNI_EXECUTION_PREPARED ((ayni_execution_phase_t)1)
/* Permit may have reached the executor; stop remains unconfirmed. Since 0.35. */
#define AYNI_EXECUTION_DISPATCHING ((ayni_execution_phase_t)2)
/* Exact local stop measurement was committed; resources release after durability. Since 0.35. */
#define AYNI_EXECUTION_STOPPED ((ayni_execution_phase_t)3)
/* Authoritative local gate never accepted this permit; no output began. Since 0.35. */
#define AYNI_EXECUTION_NOT_STARTED ((ayni_execution_phase_t)4)

/* Copied call-owned execution facts, separate from the Provider outcome. Since 0.35. */
typedef struct ayni_execution_info_v1 {
    uint32_t struct_size;         /* Caller supplies exact sizeof. */
    ayni_execution_phase_t phase; /* Explicit persisted execution/stop state. */
    uint64_t logical_call_id;     /* Exact retained call. */
    uint64_t operation_id;        /* Original action identity. */
    uint64_t permit_id;           /* Original execution attempt ID; zero before its durable dispatch
                                     preparation. */
    uint64_t requires_seq;        /* Last persisted-fact publication barrier. */
    uint32_t execution_ended;     /* Provider terminal recorded, independent of stop or target. */
    ayni_execution_target_t target;       /* Trusted measured result, UNVERIFIED is not success. */
    ayni_control_stop_cause_t stop_cause; /* Explicit measured local gate reason, or NONE. */
    ayni_status_t executor_status; /* Latest live port status; OK when no retained lease, never stop
                                      evidence. */
} ayni_execution_info_v1;

/* CONFIGURING owner-thread registrations; source descriptors are copied. Since 0.35. */
ayni_status_t ayni_register_executor(ayni_kernel_t *kernel, const ayni_executor_v1 *executor);
ayni_status_t ayni_register_execution_binding(ayni_kernel_t *kernel,
                                              const ayni_execution_binding_v1 *binding);
/* Owner-thread query; NOT_FOUND for a nonphysical or absent call. Since 0.35. */
ayni_status_t ayni_get_execution(const ayni_kernel_t *kernel, uint64_t logical_call_id,
                                 ayni_execution_info_v1 *out_info);
#ifdef __cplusplus
}
#endif
#endif
