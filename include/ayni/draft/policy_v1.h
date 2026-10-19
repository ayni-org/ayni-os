/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols.
 *
 * Strategy and GoalType interfaces as bounded decision functions
 * (contracts/host/policy_interface.md, ADR-0006). The kernel calls decide()
 * inside ayni_step on the owner thread with a read-only snapshot; the
 * callback returns bounded intents and a new private checkpoint and must not
 * call kernel functions, perform I/O or block (C-POLICY-001). This shape
 * serves deterministic replay and strategy replacement; it is not a security
 * boundary.
 */
#ifndef AYNI_DRAFT_POLICY_V1_H
#define AYNI_DRAFT_POLICY_V1_H

#include <ayni/ayni.h>
#include <ayni/draft/types_v1.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque read-only task view; valid only during the callback (C-POLICY-004).
 * Accessors are defined in M1 together with the snapshot encoding. */
typedef struct ayni_task_snapshot ayni_task_snapshot_t;

typedef uint32_t ayni_policy_intent_kind_t;

#define AYNI_POLICY_PROPOSE_GOAL ((ayni_policy_intent_kind_t)1)
#define AYNI_POLICY_REQUEST_CALL ((ayni_policy_intent_kind_t)2)
#define AYNI_POLICY_REQUEST_MODEL ((ayni_policy_intent_kind_t)3)
#define AYNI_POLICY_REPORT_PROGRESS ((ayni_policy_intent_kind_t)4)
#define AYNI_POLICY_REPORT_COMPLETION ((ayni_policy_intent_kind_t)5)
#define AYNI_POLICY_REPORT_FAILURE ((ayni_policy_intent_kind_t)6)
#define AYNI_POLICY_WRITE_MEMORY ((ayni_policy_intent_kind_t)7)

typedef struct ayni_policy_intent_v1 {
    uint32_t struct_size;
    ayni_policy_intent_kind_t kind;
    ayni_string_v1 step_key;    /* unique in the task, stable across strategies */
    uint32_t expected_revision; /* C-COMMIT-012 */
    uint32_t dependency_count;
    const ayni_object_ref_v1 *dependencies; /* versions the decision relied on */
    ayni_bytes_v1 body;
} ayni_policy_intent_v1;

/*
 * Output buffer owned by the kernel. The callback writes at most
 * intent_capacity intents and at most checkpoint_capacity checkpoint bytes;
 * exceeding either is reported by returning AYNI_CAPACITY_EXCEEDED, which
 * pauses the task with that reason (C-POLICY-002).
 */
typedef struct ayni_policy_output_v1 {
    uint32_t struct_size;
    uint32_t intent_capacity;
    uint32_t intent_count;
    uint32_t checkpoint_capacity;
    uint32_t checkpoint_size;
    uint32_t reserved; /* 0 */
    ayni_policy_intent_v1 *intents;
    uint8_t *checkpoint;
} ayni_policy_output_v1;

/* Private checkpoint as last returned by this strategy (C-POLICY-007). */
typedef struct ayni_private_checkpoint_v1 {
    ayni_string_v1 strategy_id;
    uint32_t strategy_version;
    uint32_t reserved; /* 0 */
    ayni_bytes_v1 state;
} ayni_private_checkpoint_v1;

typedef ayni_status_t (*ayni_decide_fn)(void *ctx, const ayni_task_snapshot_t *snapshot,
                                        const ayni_private_checkpoint_v1 *checkpoint,
                                        ayni_policy_output_v1 *output);

typedef struct ayni_strategy_v1 {
    uint32_t struct_size;
    uint32_t version;
    ayni_string_v1 strategy_id;
    void *ctx; /* caller-owned until the kernel reaches STOPPED */
    uint32_t max_intents_per_call;
    uint32_t max_checkpoint_bytes;
    ayni_decide_fn decide;
} ayni_strategy_v1;

typedef struct ayni_goal_type_v1 {
    uint32_t struct_size;
    uint32_t version;
    ayni_string_v1 goal_type_id; /* e.g. "reminder" */
    ayni_string_v1 default_strategy_id;
    uint32_t max_wait_ms; /* wait limit before defer/expire/pause, C-RES-005 */
    uint32_t reserved;    /* 0 */
    void *ctx;
    ayni_decide_fn decide; /* completion and pause-point decisions */
} ayni_goal_type_v1;

ayni_status_t ayni_register_strategy(ayni_kernel_t *kernel, const ayni_strategy_v1 *strategy);
ayni_status_t ayni_register_goal_type(ayni_kernel_t *kernel, const ayni_goal_type_v1 *goal_type);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_POLICY_V1_H */
