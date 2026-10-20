/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols.
 *
 * Planned parts of the strategy interface beyond ayni.h
 * (contracts/host/policy_interface.md, ADR-0006, ADR-0022). Strategies,
 * their registration, the task snapshot and the intents that end a task are
 * implemented in ayni.h and are not redeclared here.
 */
#ifndef AYNI_DRAFT_POLICY_V1_H
#define AYNI_DRAFT_POLICY_V1_H

#include <ayni/ayni.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Intent kinds planned next to AYNI_POLICY_REPORT_COMPLETION and
 * AYNI_POLICY_REPORT_FAILURE. A decision may then hold several intents, each
 * with a step_key unique in the task and stable across strategies, the
 * object versions the decision relied on as dependencies, and a body; these
 * fields are appended to ayni_policy_intent_v1 when the kinds are
 * implemented. */
#define AYNI_POLICY_PROPOSE_GOAL ((ayni_policy_intent_kind_t)1)
#define AYNI_POLICY_REQUEST_CALL ((ayni_policy_intent_kind_t)2)
#define AYNI_POLICY_REQUEST_MODEL ((ayni_policy_intent_kind_t)3)
#define AYNI_POLICY_REPORT_PROGRESS ((ayni_policy_intent_kind_t)4)
#define AYNI_POLICY_WRITE_MEMORY ((ayni_policy_intent_kind_t)7)

/*
 * Private checkpoint as last returned by this strategy (C-POLICY-007). decide
 * is planned to receive it next to the snapshot, and ayni_policy_output_v1 to
 * gain checkpoint_capacity, checkpoint_size and the checkpoint bytes, bounded
 * by a max_checkpoint_bytes appended to ayni_strategy_v1; exceeding it is
 * reported by returning AYNI_CAPACITY_EXCEEDED (C-POLICY-002).
 */
typedef struct ayni_private_checkpoint_v1 {
    ayni_string_v1 strategy_id;
    uint32_t strategy_version;
    uint32_t reserved; /* 0 */
    ayni_bytes_v1 state;
} ayni_private_checkpoint_v1;

/*
 * The goal type fields still planned (max_wait_ms before defer, expire or
 * pause per C-RES-005, and a decide function for pause-point decisions) are
 * appended to ayni_goal_type_v1 when resource waits are implemented.
 */

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_POLICY_V1_H */
