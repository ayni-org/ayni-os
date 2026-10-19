/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols.
 *
 * Execution permits exchanged between a control domain's authority-owner
 * kernel (issuer) and an executor (contracts/domain/permits_and_stop.md,
 * ADR-0001). The executor side lives in control/ and safety/, which do not
 * depend on core/; this header is the shared record format between them.
 *
 * Deadline rule enforced by the executor (C-PERMIT-006): for a permit or
 * renewal echoing nonce N, issued by the executor at local monotonic time
 * t_N, deadline = max(deadline, t_N + lease_us). Arrival time never anchors
 * the deadline; unknown or evicted nonces are rejected.
 */
#ifndef AYNI_DRAFT_CONTROL_V1_H
#define AYNI_DRAFT_CONTROL_V1_H

#include <ayni/ayni.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ayni_execution_form_t;

#define AYNI_FORM_PROCEDURE ((ayni_execution_form_t)1) /* not renewable */
#define AYNI_FORM_SESSION ((ayni_execution_form_t)2)   /* short renewable lease */

/* All must equal the executor's current values (C-PERMIT-004). */
typedef struct ayni_permit_epochs_v1 {
    uint64_t kernel_boot_epoch;
    uint64_t authority_epoch;
    uint64_t executor_boot_epoch;
    uint64_t safety_epoch;
    uint32_t calibration_version;
    uint32_t reserved; /* 0 */
} ayni_permit_epochs_v1;

/* Nonce announced by the executor; the executor alone records t_N. */
typedef struct ayni_executor_nonce_v1 {
    uint64_t control_domain_id;
    uint64_t nonce;
} ayni_executor_nonce_v1;

typedef struct ayni_execution_permit_v1 {
    uint32_t struct_size;
    ayni_execution_form_t form;
    uint64_t permit_id;
    uint64_t operation_id;
    uint64_t control_domain_id;
    ayni_permit_epochs_v1 epochs;
    uint64_t echoed_nonce;
    uint32_t control_mode;
    uint32_t lease_us; /* L, declared by the capability, includes clock drift margin */
    uint32_t max_command_age_us;
    uint32_t reserved;           /* 0 */
    uint8_t envelope_digest[32]; /* over the envelope's domain fields, C-VER-032 */
} ayni_execution_permit_v1;

/* Renews a session permit; procedures are never renewed (C-PERMIT-010). */
typedef struct ayni_permit_renewal_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* 0 */
    uint64_t permit_id;
    uint64_t echoed_nonce;
    ayni_permit_epochs_v1 epochs;
} ayni_permit_renewal_v1;

/*
 * Facts about stopping and outcome, recorded separately (C-STOP-006,
 * C-STOP-007). Only STOP_CONFIRMED, backed by local measurement, proves the
 * physical output stopped.
 */
typedef uint32_t ayni_stop_fact_t;

#define AYNI_STOP_CANCEL_ACCEPTED ((ayni_stop_fact_t)1)
#define AYNI_STOP_COMMAND_DELIVERED ((ayni_stop_fact_t)2)
#define AYNI_STOP_CONFIRMED ((ayni_stop_fact_t)3)

typedef uint32_t ayni_target_verification_t;

#define AYNI_TARGET_VERIFIED ((ayni_target_verification_t)1)
#define AYNI_TARGET_ESTIMATED ((ayni_target_verification_t)2)
#define AYNI_TARGET_UNKNOWN ((ayni_target_verification_t)3)

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_CONTROL_V1_H */
