/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols.
 *
 * Planned parts of the strategy interface beyond ayni.h
 * (contracts/host/policy_interface.md, ADR-0006, ADR-0022, ADR-0024).
 * Strategies, their registration, the task snapshot with its steps and
 * private checkpoint, and the intents that report progress and end a task
 * are implemented in ayni.h and are not redeclared here.
 */
#ifndef AYNI_DRAFT_POLICY_V1_H
#define AYNI_DRAFT_POLICY_V1_H

#include <ayni/ayni.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Intent kinds planned next to AYNI_POLICY_REPORT_PROGRESS,
 * AYNI_POLICY_REPORT_COMPLETION and AYNI_POLICY_REPORT_FAILURE. A call is
 * requested for a step named by step_key. The object versions the decision
 * relied on, as dependencies, and a body are appended to
 * ayni_policy_intent_v1 when the kinds are implemented; the step's call
 * identity, binding, input digest, result reference and unknown state are
 * appended to ayni_task_step_v1 (C-CALL-004, C-ID-003). */
#define AYNI_POLICY_PROPOSE_GOAL ((ayni_policy_intent_kind_t)1)
#define AYNI_POLICY_REQUEST_CALL ((ayni_policy_intent_kind_t)2)
#define AYNI_POLICY_REQUEST_MODEL ((ayni_policy_intent_kind_t)3)
#define AYNI_POLICY_WRITE_MEMORY ((ayni_policy_intent_kind_t)7)

/*
 * The goal type fields still planned (max_wait_ms before defer, expire or
 * pause per C-RES-005, and a decide function for pause-point decisions) are
 * appended to ayni_goal_type_v1 when resource waits are implemented.
 */

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_POLICY_V1_H */
