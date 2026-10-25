/*
 * DRAFT stop/target facts (C-VER-007): no symbols. Permit types and
 * the local executor gate are published in <ayni/control.h>.
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

#include <ayni/control.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Permit types and local gate are implemented in the formal control header. */

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
