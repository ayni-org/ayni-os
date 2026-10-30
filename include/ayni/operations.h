/* Task-owned managed operation reads and staged controls. Since 0.51. */
#ifndef AYNI_OPERATIONS_H
#define AYNI_OPERATIONS_H
#include <ayni/calls.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Hard maximum for one explicit model wait; no operation cancellation implied. Since 0.51. */
#define AYNI_OPERATION_WAIT_MS_MAX 30000u
/* Copied exact logical identity; not a capability token. Since 0.51. */
typedef struct ayni_operation_ref_v1 {
    uint64_t task_id;         /* Positive immutable owning Task. */
    uint64_t logical_call_id; /* Positive original Call, never a slot. */
    uint64_t operation_id;    /* Positive action identity or subscription opening. */
} ayni_operation_ref_v1;
/* Copied observed frontier, independent of same-state lease ACKs. Since 0.51. */
typedef struct ayni_operation_frontier_v1 {
    uint64_t boot_epoch;       /* Positive observation boot; changed boot interrupts waiting. */
    uint64_t sequence;         /* Progress/sample sequence; zero before the first. */
    uint64_t gaps;             /* Known sample discontinuities; zero for an action. */
    uint32_t phase;            /* Call phase for action; subscription phase otherwise. */
    uint32_t cancel_requested; /* Exact 0/1 local cancellation fact, not stopped evidence. */
    uint32_t cancel_result;    /* Zero or the original classified cancellation response. */
    uint32_t outcome;          /* Zero pending, otherwise original Call terminal outcome. */
} ayni_operation_frontier_v1;
/* Pure value equality; owns the comparison shared by Core and codecs. Since 0.51. */
static inline int ayni_operation_frontier_equal(const ayni_operation_frontier_v1 *left,
                                                const ayni_operation_frontier_v1 *right) {
    return left && right && left->boot_epoch == right->boot_epoch &&
           left->sequence == right->sequence && left->gaps == right->gaps &&
           left->phase == right->phase && left->cancel_requested == right->cancel_requested &&
           left->cancel_result == right->cancel_result && left->outcome == right->outcome;
}
/* One Task-scoped copied projection; sample carries no raw media. Since 0.51. */
typedef struct ayni_operation_info_v1 {
    uint32_t struct_size;                   /* Exact sizeof this ABI layout. */
    ayni_call_form_t form;                  /* ACTION or SUBSCRIPTION. */
    ayni_operation_ref_v1 reference;        /* Current exact operation/opening. */
    ayni_operation_frontier_v1 frontier;    /* Return unchanged with wait. */
    ayni_catalog_name_v1 step_key;          /* Copied original step; result uses the same key. */
    ayni_call_info_v1 call;                 /* Original ledger/diagnostics, including durability. */
    ayni_subscription_info_v1 subscription; /* struct_size zero for action. */
    ayni_observation_info_v1 sample;        /* struct_size zero without an available sample. */
} ayni_operation_info_v1;
/* Wait at an explicit observed frontier; no busy polling. Since 0.51. */
#define AYNI_OPERATION_WAIT 1u
/* Request cancellation of this Task's action; not terminal success. Since 0.51. */
#define AYNI_OPERATION_CANCEL 2u
/* Permanently close this Task's subscription intent. Since 0.51. */
#define AYNI_OPERATION_CLOSE 3u
/* Callback-scoped input copied by control_operation before returning. Since 0.51. */
typedef struct ayni_operation_control_v1 {
    uint32_t struct_size;             /* Exact sizeof. */
    uint32_t kind;                    /* WAIT, CANCEL or CLOSE. */
    ayni_string_v1 step_key;          /* New stable control-step identity, not the target. */
    ayni_operation_ref_v1 reference;  /* Original Task/Call and exact opening. */
    ayni_operation_frontier_v1 after; /* WAIT only; all zero for other controls. */
    uint32_t timeout_ms;              /* Positive bounded WAIT duration; zero otherwise. */
    uint32_t reserved;                /* Must be zero. */
} ayni_operation_control_v1;
#ifdef __cplusplus
}
#endif
#endif
