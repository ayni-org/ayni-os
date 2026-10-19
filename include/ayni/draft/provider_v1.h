/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols.
 *
 * Provider function table (contracts/host/provider.md). A Provider
 * implements capabilities; it is trusted built-in code but its output is
 * untrusted data that the kernel validates (C-TRUST-001). submit, cancel and
 * query run on the kernel owner thread and must not block; results come back
 * through the completion reserved for the attempt.
 */
#ifndef AYNI_DRAFT_PROVIDER_V1_H
#define AYNI_DRAFT_PROVIDER_V1_H

#include <ayni/ayni.h>
#include <ayni/draft/host_v1.h>
#include <ayni/draft/kernel_api.h>
#include <ayni/draft/types_v1.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One attempt of a logical call (C-CALL-002). */
typedef struct ayni_provider_request_v1 {
    uint32_t struct_size;
    uint32_t capability_version;
    ayni_attempt_ref_v1 attempt;
    uint64_t logical_call_id;
    uint64_t operation_id; /* 0 for calls without side effects */
    ayni_string_v1 capability_id;
    ayni_bytes_v1 input; /* validated against the input schema; media by reference only */
} ayni_provider_request_v1;

typedef uint32_t ayni_call_outcome_t;

/* Terminal: exactly one per attempt is applied. */
#define AYNI_CALL_COMPLETED ((ayni_call_outcome_t)1)
#define AYNI_CALL_FAILED ((ayni_call_outcome_t)2)
#define AYNI_CALL_CANCELLED ((ayni_call_outcome_t)3)
/* Non-terminal: the attempt may still complete afterwards. */
#define AYNI_CALL_PROGRESS ((ayni_call_outcome_t)4)
#define AYNI_CALL_CANNOT_CANCEL ((ayni_call_outcome_t)5)
#define AYNI_CALL_CANCEL_UNKNOWN ((ayni_call_outcome_t)6)

/* Query results; NOT_ACCEPTED only from AYNI_QUERY_AUTHORITATIVE providers
 * (C-PROV-005). */
#define AYNI_QUERY_RESULT_FOUND 1u
#define AYNI_QUERY_RESULT_NOT_ACCEPTED 2u
#define AYNI_QUERY_RESULT_UNKNOWN 3u

/* Cost facts reported with every terminal result, late ones included
 * (C-PROV-007). */
typedef struct ayni_cost_v1 {
    uint32_t calls;
    uint32_t reserved; /* 0 */
    uint64_t tokens;
    uint64_t bytes;
} ayni_cost_v1;

/*
 * Result posted by the Provider for an attempt, copied by the kernel before
 * the posting call returns. sequence orders progress results.
 */
typedef struct ayni_provider_result_v1 {
    uint32_t struct_size;
    ayni_call_outcome_t outcome;
    ayni_attempt_ref_v1 attempt;
    uint64_t sequence;
    uint32_t reason;       /* reason code when FAILED; never vendor text */
    uint32_t query_result; /* AYNI_QUERY_RESULT_* on a query completion; 0 otherwise */
    ayni_cost_v1 cost;
    ayni_bytes_v1 result; /* checked against the result schema, C-PROV-003 */
} ayni_provider_result_v1;

/*
 * Posts a result on the completion reserved for an attempt. Any non-ISR
 * thread. A completion takes any number of non-terminal results, of which
 * only the latest not yet applied is kept, and one terminal result, which
 * releases it; a later terminal post is ignored (C-CALL-012).
 */
ayni_status_t ayni_provider_post(ayni_completion_t *completion,
                                 const ayni_provider_result_v1 *result);

typedef struct ayni_provider_v1 {
    uint32_t struct_size;
    uint32_t version;
    ayni_string_v1 provider_id;
    void *ctx; /* caller-owned until the kernel reaches STOPPED */

    uint32_t capability_count;
    uint32_t reserved;                           /* 0 */
    const ayni_capability_desc_v1 *capabilities; /* what this provider implements */

    ayni_submit_result_t (*submit)(void *ctx, const ayni_provider_request_v1 *request,
                                   ayni_completion_t *completion);
    /* A request; the outcome arrives on the attempt's completion (C-PROV-006). */
    ayni_submit_result_t (*cancel)(void *ctx, ayni_attempt_ref_v1 attempt);
    /* NULL unless a capability declares a query authority other than NONE. */
    ayni_submit_result_t (*query)(void *ctx, uint64_t operation_id, ayni_completion_t *completion);
} ayni_provider_v1;

ayni_status_t ayni_register_provider(ayni_kernel_t *kernel, const ayni_provider_v1 *provider);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_PROVIDER_V1_H */
