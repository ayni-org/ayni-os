/*
 * Asynchronous bounded RAM audit ring (Since 0.60, ADR-0113).
 *
 * Captures high-value security, preemption, execution-quarantine, reflex,
 * lifecycle and Call-terminal facts (including RAM-only EPHEMERAL Calls) in a
 * fixed arena ring without blocking Core step() or touching the synchronous WAL.
 * When full, the oldest unread record is overwritten and dropped_overflow rises.
 * Host/runtime consumers drain batches asynchronously via ayni_take_audit_records().
 */
#ifndef AYNI_AUDIT_H
#define AYNI_AUDIT_H

#include <ayni/ayni.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Categories of Kernel-emitted audit records. Since 0.60. */
typedef uint32_t ayni_audit_kind_t;

/* Call reached a terminal outcome (both EPHEMERAL and DURABLE). Since 0.60. */
#define AYNI_AUDIT_CALL_TERMINAL ((ayni_audit_kind_t)1)
/* Capability, Grant, tool policy, or Scene authorization denied. Since 0.60. */
#define AYNI_AUDIT_AUTHORITY_DENIED ((ayni_audit_kind_t)2)
/* Held EPHEMERAL Call preempted by higher-priority Call or ReflexRule. Since 0.60. */
#define AYNI_AUDIT_RESOURCE_PREEMPTED ((ayni_audit_kind_t)3)
/* Physical actuator lease entered quarantine/stop proof tracking. Since 0.60. */
#define AYNI_AUDIT_ACTUATOR_QUARANTINE ((ayni_audit_kind_t)4)
/* Physical actuator lease quarantine released after measured stop proof. Since 0.60. */
#define AYNI_AUDIT_ACTUATOR_RELEASE ((ayni_audit_kind_t)5)
/* Deterministic ReflexRule fired or was denied. Since 0.60. */
#define AYNI_AUDIT_REFLEX_EVENT ((ayni_audit_kind_t)6)
/* Kernel lifecycle transitioned (e.g. RUNNING, RESTRICTED, DRAINING, STOPPED). Since 0.60. */
#define AYNI_AUDIT_LIFECYCLE ((ayni_audit_kind_t)7)

/* Bit flags carried in ayni_audit_record_v1.flags. Since 0.60. */
/* Associated Call or action was Tier 0 RAM-only EPHEMERAL. Since 0.60. */
#define AYNI_AUDIT_FLAG_EPHEMERAL ((uint32_t)(1u << 0))
/* Associated Call or lease governed a physical actuator. Since 0.60. */
#define AYNI_AUDIT_FLAG_PHYSICAL ((uint32_t)(1u << 1))
/* Action involved preemption or reflex soft-stop. Since 0.60. */
#define AYNI_AUDIT_FLAG_PREEMPTED ((uint32_t)(1u << 2))
/* Action was denied by authority, Scene, or reflex policy. Since 0.60. */
#define AYNI_AUDIT_FLAG_DENIED ((uint32_t)(1u << 3))

/*
 * One immutable fixed-width audit record (104 bytes, zero implicit padding).
 * label holds only static catalog/capability/resource/rule identifiers (never
 * user text or secrets); unused trailing bytes are zeroed. Since 0.60.
 */
typedef struct ayni_audit_record_v1 {
    uint32_t struct_size;                  /* sizeof(ayni_audit_record_v1) */
    ayni_audit_kind_t kind;                /* AYNI_AUDIT_* category */
    uint64_t seq;                          /* 1-based monotonic boot sequence; gaps prove drop */
    uint64_t boot_epoch;                   /* Kernel boot epoch when emitted (0 pre-recovery) */
    uint64_t at_us;                        /* Host monotonic timestamp in microseconds */
    uint64_t subject_id;                   /* Primary ID (call_id, task_id, or 0) */
    uint64_t auxiliary_id;                 /* Secondary ID (task_id, preempting_call, permit, 0) */
    ayni_status_t status;                  /* Associated public status code */
    ayni_reason_t reason;                  /* Associated public reason or restriction code */
    uint32_t detail_code;                  /* Kind-specific enum (outcome, action, lifecycle) */
    uint32_t flags;                        /* AYNI_AUDIT_FLAG_* bitmask */
    uint32_t label_size;                   /* Valid bytes in label, 0..AYNI_CATALOG_ID_MAX_BYTES */
    uint32_t reserved;                     /* Always 0 */
    char label[AYNI_CATALOG_ID_MAX_BYTES]; /* Static identifier, zero-padded, not terminated */
} ayni_audit_record_v1;

/*
 * Current audit ring capacity and drop diagnostics (40 bytes, zero implicit padding).
 * Since 0.60.
 */
typedef struct ayni_audit_info_v1 {
    uint32_t struct_size;      /* sizeof(ayni_audit_info_v1) */
    uint32_t capacity;         /* Configured ring slots; 0 when disabled */
    uint32_t pending;          /* Unread records currently retained in the ring */
    uint32_t reserved;         /* Always 0 */
    uint64_t next_seq;         /* Next sequence to assign (total emitted = next_seq - 1) */
    uint64_t dropped_overflow; /* Oldest unread records overwritten before being drained */
    uint64_t drained_total;    /* Cumulative records delivered via ayni_take_audit_records */
} ayni_audit_info_v1;

/*
 * Copies current audit ring counters into out. Owner thread only; performs no I/O.
 * Returns AYNI_OK, AYNI_INVALID_ARGUMENT, AYNI_UNSUPPORTED_VERSION, or AYNI_INVALID_STATE.
 * Since 0.60.
 */
ayni_status_t ayni_get_audit_info(const ayni_kernel_t *kernel, ayni_audit_info_v1 *out);

/*
 * Drains up to capacity oldest unread audit records in FIFO seq order into out.
 * Sets *out_count to the number of records copied (0 when empty or disabled).
 * Owner thread only; performs no I/O and never touches the WAL.
 * Returns AYNI_OK, AYNI_INVALID_ARGUMENT, or AYNI_INVALID_STATE.
 * Since 0.60.
 */
ayni_status_t ayni_take_audit_records(ayni_kernel_t *kernel, ayni_audit_record_v1 *out,
                                      uint32_t capacity, uint32_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_AUDIT_H */
