/* Managed calls and trusted Provider configuration, Since 0.31.
 * Registry descriptors are copied during CONFIGURING. Callback contexts and
 * code remain caller-owned through successful drain and destruction. A catalog
 * match grants no authority and never starts I/O.
 */
#ifndef AYNI_CALLS_H
#define AYNI_CALLS_H

#include <ayni/authority.h>
#include <ayni/control.h>
#include <ayni/resources.h>
#include <ayni/subscriptions.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ayni_call_form_t;
/* One result without a physical control session. Since 0.31. */
#define AYNI_CALL_UNARY ((ayni_call_form_t)1)
/* One tracked operation, whose effects are recorded independently. Since 0.31. */
#define AYNI_CALL_ACTION ((ayni_call_form_t)2)
/* Finite Task-owned collection lease; implemented Since 0.43. Declared Since 0.31. */
#define AYNI_CALL_SUBSCRIPTION ((ayni_call_form_t)3)
typedef uint32_t ayni_call_class_t;
/* Ordinary capability, still subject to all declared effects. Since 0.31. */
#define AYNI_CALL_TOOL ((ayni_call_class_t)1)
/* Every accepted logical request consumes the Task model count. Since 0.31. */
#define AYNI_CALL_MODEL ((ayni_call_class_t)2)
/* May send data outside this device. Since 0.31. */
#define AYNI_CALL_EFFECT_EGRESS 0x1u
/* May incur an external charge; no local balance is maintained. Since 0.31. */
#define AYNI_CALL_EFFECT_BILLING 0x2u
/* Presents content to a user. Since 0.31. */
#define AYNI_CALL_EFFECT_NOTIFICATION 0x4u
/* Requires execution permits before hardware use. Since 0.31. */
#define AYNI_CALL_EFFECT_PHYSICAL 0x8u
typedef uint32_t ayni_call_idempotency_t;
/* Repetition is semantically idempotent, not permission to retry. Since 0.31. */
#define AYNI_CALL_IDEMPOTENT ((ayni_call_idempotency_t)1)
/* Repetition may cause another effect. Since 0.31. */
#define AYNI_CALL_NOT_IDEMPOTENT ((ayni_call_idempotency_t)2)
typedef uint32_t ayni_call_query_authority_t;
/* No remote outcome query. Since 0.31. */
#define AYNI_CALL_QUERY_NONE ((ayni_call_query_authority_t)1)
/* An absent result proves nothing about execution. Since 0.31. */
#define AYNI_CALL_QUERY_BEST_EFFORT ((ayni_call_query_authority_t)2)
/* Service can authoritatively prove an operation outcome. Since 0.31. */
#define AYNI_CALL_QUERY_AUTHORITATIVE ((ayni_call_query_authority_t)3)
typedef uint32_t ayni_call_cancel_mode_t;
/* No remote cancellation; drain still owes a terminal receipt. Since 0.31. */
#define AYNI_CALL_CANCEL_NONE ((ayni_call_cancel_mode_t)1)
/* Requests cancellation without promising that it stopped execution. Since 0.31. */
#define AYNI_CALL_CANCEL_REQUEST ((ayni_call_cancel_mode_t)2)
/* Can confirm remote cancellation; distinct from actuator stop. Since 0.31. */
#define AYNI_CALL_CANCEL_CONFIRMED ((ayni_call_cancel_mode_t)3)
/* Complete singleton authority fields followed by UTF-8 text. Since 0.31. */
#define AYNI_CALL_INPUT_FIELDS_TEXT 1u
/* Bounded UTF-8 text, including empty text. Since 0.31. */
#define AYNI_CALL_RESULT_TEXT 1u
/* Final UTF-8 text, or a list of typed tool calls for the strategy. MODEL
 * class only, never PHYSICAL or subscription (ADR-0054). Since 0.37. */
#define AYNI_CALL_RESULT_TOOL_CALLS 2u
/* Maximum tool calls in one result. Since 0.37. */
#define AYNI_CALL_TOOL_COUNT_MAX 8u
/* Maximum members of one atomic resource group. Since 0.31. */
#define AYNI_CALL_RESOURCE_COUNT_MAX AYNI_RESOURCE_GROUP_COUNT_MAX
/* Maximum supported text bytes per input or result. Since 0.31. */
#define AYNI_CALL_TEXT_BYTES_MAX 65536u
/* Maximum distinct capability versions advertised by one Provider. Since 0.31. */
#define AYNI_PROVIDER_CAPABILITY_COUNT_MAX 8u
/* Maximum registered execution capability versions. Since 0.31. */
#define AYNI_CALL_CAPABILITY_CAPACITY_MAX 256u
/* Maximum registered Provider versions. Since 0.31. */
#define AYNI_PROVIDER_CAPACITY_MAX 64u
/* Maximum immutable Binding versions. Since 0.31. */
#define AYNI_BINDING_CAPACITY_MAX 256u

/* Immutable resource demand; names resolve at start, never by registry order. */
/* Since 0.31. Explicit trusted Binding selection and concrete typed arguments.
 * All views are copied by output.request_call before it returns. The model may
 * propose values, but cannot register a new Binding or endpoint. */
typedef struct ayni_call_request_v1 {
    uint32_t struct_size;
    ayni_policy_intent_kind_t kind; /* REQUEST_CALL or REQUEST_MODEL; matches contract class. */
    uint32_t binding_version;       /* Nonzero exact version. */
    uint32_t reserved;              /* Must be zero. */
    ayni_string_v1 step_key;        /* Stable nonempty Task-local identity. */
    ayni_string_v1 binding_id;      /* Exact trusted selection. */
    ayni_scope_v1 arguments;        /* Every registered authority field, one concrete value each. */
    ayni_string_v1 text;            /* Bounded UTF-8 business body, never credentials. */
} ayni_call_request_v1;

/* Since 0.37. One call a model proposes: an offered tool's exact Binding,
 * every authority field with one concrete value, and an optional body for
 * that tool's input text. A proposal only: the strategy decides whether to
 * request it, and the request is checked like any other (ADR-0054). */
typedef struct ayni_tool_call_v1 {
    uint32_t struct_size;
    uint32_t binding_version;  /* Nonzero exact version. */
    ayni_string_v1 binding_id; /* Catalog id of an offered tool. */
    ayni_scope_v1 arguments;   /* Every field once, one value each, in any order. */
    ayni_string_v1 text;       /* UTF-8 body within the tool's input text limit. */
} ayni_tool_call_v1;

/* Since 0.31. A strategy sees only its Task's original call and result. A nonzero
 * requires_seq must be covered by the snapshot's durable prefix before its body
 * can be used for a further external call; the Kernel enforces that gate. */
typedef struct ayni_task_call_result_v1 {
    uint32_t struct_size;
    uint32_t outcome;         /* Zero pending; otherwise AYNI_CALL_* terminal value. */
    uint64_t logical_call_id; /* Stable nonzero identity. */
    uint64_t operation_id;    /* Current generation operation; stable for ordinary calls. */
    uint64_t requires_seq;    /* Last result commit sequence; zero before terminal. */
    ayni_status_t status;     /* Classified result status. */
    ayni_reason_t reason;     /* Classified result cause. */
    ayni_string_v1 text;      /* Borrowed only during decide; empty while pending/unknown. */
    /* Since 0.37. Nonzero only for a completed TOOL_CALLS result that holds
     * tool calls instead of text; read each with snapshot call_tool. */
    uint32_t tool_call_count;
    uint32_t reserved; /* Written zero. */
} ayni_task_call_result_v1;

/* Since 0.37. Caller storage for one decoded tool call. call.arguments.fields
 * points into fields and each token set into tokens, so use out in place
 * (copying the struct keeps pointers to the original); strings borrow Core
 * memory until decide returns. To run it, a strategy copies call's binding,
 * arguments and text into an ayni_call_request_v1 of kind REQUEST_CALL. */
typedef struct ayni_task_tool_call_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* Written zero. */
    ayni_tool_call_v1 call;
    ayni_scope_field_v1 fields[AYNI_AUTHORITY_FIELD_COUNT_MAX];
    ayni_string_v1 tokens[AYNI_AUTHORITY_FIELD_COUNT_MAX];
} ayni_task_tool_call_v1;

typedef struct ayni_call_resource_v1 {
    ayni_string_v1 resource_id; /* Nonempty catalog ID; unique within the group. */
    uint32_t units;             /* Positive units, bounded by resource capacity. */
    uint32_t reserved;          /* Zero. */
} ayni_call_resource_v1;

/* Execution semantics of one exact capability version. Independent Provider
 * advertisements must match every field, including limits and resources.
 */
typedef struct ayni_call_contract_v1 {
    uint32_t struct_size;                /* sizeof this structure. */
    ayni_call_form_t form;               /* One explicit call form; zero is invalid. */
    ayni_call_class_t category;          /* TOOL or MODEL; never inferred from name. */
    uint32_t effects;                    /* Zero or CALL_EFFECT flags. */
    ayni_call_idempotency_t idempotency; /* Repeatability, not automatic retry policy. */
    ayni_call_query_authority_t query;   /* Remote evidence strength. */
    ayni_call_cancel_mode_t cancel;      /* Remote cancel support. */
    uint32_t dedupe_window_ms;           /* Zero: no server operation-ID deduplication. */
    uint32_t timeout_ms;                 /* Positive request duration, not queue wait. */
    uint32_t input_format;               /* CALL_INPUT_FIELDS_TEXT in this version. */
    uint32_t result_format;              /* CALL_RESULT_TEXT or, since 0.37, TOOL_CALLS. */
    uint32_t input_text_limit;           /* Bytes, 0..CALL_TEXT_BYTES_MAX. */
    uint32_t result_text_limit; /* Bytes, 0..CALL_TEXT_BYTES_MAX; also bounds a tool list. */
    uint32_t resource_count;    /* 0..CALL_RESOURCE_COUNT_MAX. */
    const ayni_call_resource_v1 *resources; /* Null only when resource_count is zero. */
    /* Since 0.43. Required for SUBSCRIPTION, null for every other form. Copied. */
    const ayni_subscription_contract_v1 *subscription;
} ayni_call_contract_v1;

/* Supplements the authority descriptor of the same exact ID/version. */
typedef struct ayni_call_capability_v1 {
    uint32_t struct_size;           /* sizeof this structure. */
    uint32_t version;               /* Positive business semantic version. */
    ayni_string_v1 capability_id;   /* Nonempty catalog identity. */
    ayni_call_contract_v1 contract; /* Copied immutable execution declaration. */
} ayni_call_capability_v1;

/* Opaque delivery target. Unlike Host completion slots, retains cancel,
 * progress and terminal independently. Producers keep the original attempt.
 */
typedef struct ayni_provider_completion ayni_provider_completion_t;

typedef uint32_t ayni_call_outcome_t;
/* Terminal: operation completed with a validated result. Since 0.31. */
#define AYNI_CALL_COMPLETED ((ayni_call_outcome_t)1)
/* Terminal: known failure; effect must still be reported explicitly. Since 0.31. */
#define AYNI_CALL_FAILED ((ayni_call_outcome_t)2)
/* Terminal: cancellation confirmed, not necessarily without effects. Since 0.31. */
#define AYNI_CALL_CANCELLED ((ayni_call_outcome_t)3)
/* Coalescible progress; sequence must increase and be positive. Since 0.31. */
#define AYNI_CALL_PROGRESS ((ayni_call_outcome_t)4)
/* Reliable control: cancellation refused, terminal result still owed. Since 0.31. */
#define AYNI_CALL_CANNOT_CANCEL ((ayni_call_outcome_t)5)
/* Reliable control: cancellation uncertain, terminal result still owed. Since 0.31. */
#define AYNI_CALL_CANCEL_UNKNOWN ((ayni_call_outcome_t)6)
/* Terminal: tracking ended without knowing remote outcome. Since 0.31. */
#define AYNI_CALL_ABANDONED ((ayni_call_outcome_t)7)
/* Core-only terminal: background resource wait expired before dispatch. Since 0.33. */
#define AYNI_CALL_SKIPPED ((ayni_call_outcome_t)8)
/* Reliable start ACK; sequence=1, no payload, keeps the delivery borrow. Since 0.43. */
#define AYNI_CALL_SUBSCRIPTION_STARTED ((ayni_call_outcome_t)9)
/* Reliable renew ACK; exact requested positive lease sequence, no payload. Since 0.43. */
#define AYNI_CALL_SUBSCRIPTION_RENEWED ((ayni_call_outcome_t)10)
typedef uint32_t ayni_call_effect_t;
/* Affirmative evidence of no external effect. Since 0.31. */
#define AYNI_CALL_EFFECT_NONE ((ayni_call_effect_t)1)
/* Some effect occurred without completing the requested outcome. Since 0.31. */
#define AYNI_CALL_EFFECT_PARTIAL ((ayni_call_effect_t)2)
/* Requested effect completed. Since 0.31. */
#define AYNI_CALL_EFFECT_COMPLETE ((ayni_call_effect_t)3)
/* Whether or how far execution happened remains unknown. Since 0.31. */
#define AYNI_CALL_EFFECT_UNKNOWN ((ayni_call_effect_t)4)
/* Since 0.31. CALL_QUERY_RESULT body; nonzero command_id and the call's
 * query_revision CAS are required. Only the bound owner's MANAGEMENT session
 * may submit it. The original execution Grant need not remain valid. A retry
 * with the same command identity returns its first acceptance without more I/O.
 */
typedef struct ayni_call_query_result_v1 {
    uint32_t struct_size;     /* sizeof this structure. */
    uint32_t reserved;        /* Must be zero. */
    uint64_t logical_call_id; /* Retained unknown outcome/effect, never an arbitrary endpoint. */
} ayni_call_query_result_v1;

/* Query found an outcome of the exact operation. Since 0.31. */
#define AYNI_QUERY_RESULT_FOUND 1u
/* Authoritative service proves this operation was never accepted. Since 0.31. */
#define AYNI_QUERY_RESULT_NOT_ACCEPTED 2u
/* Query cannot establish an outcome; never proof of nonexecution. Since 0.31. */
#define AYNI_QUERY_RESULT_UNKNOWN 3u

/* Untrusted result view, copied before post returns. Diagnostic status/reason
 * contain only public codes; result is business data, never an error log.
 * Since 0.37 a COMPLETED result of a TOOL_CALLS call holds either text or
 * 1..CALL_TOOL_COUNT_MAX tool calls with empty text, never both; every other
 * outcome has none. A list must name only tools offered with the request and
 * values inside their offered sets, otherwise the receipt is invalid Provider
 * evidence. Report undecodable model output as FAILED with reason
 * MODEL_OUTPUT instead, as when post refuses a list over result_text_limit.
 */
typedef struct ayni_provider_result_v1 {
    uint32_t struct_size;        /* sizeof this structure. */
    ayni_call_outcome_t outcome; /* One explicit delivery kind. */
    ayni_attempt_ref_v1 attempt; /* Original request identity, never read from a reused slot. */
    uint64_t sequence;           /* Positive for progress/lease ACK, zero for other receipts. */
    ayni_status_t status;        /* Classified result, not a vendor status number. */
    ayni_reason_t reason;        /* Public cause; NONE when there is no finer classification. */
    ayni_call_effect_t effect;   /* Explicit knowledge, never inferred from HTTP status. */
    uint32_t query_result;       /* Zero for submit; QUERY_RESULT value for a query. */
    ayni_string_v1 result;       /* Bounded UTF-8 text, empty for cancellation controls. */
    uint32_t tool_call_count;    /* Since 0.37. Zero, or the size of tool_calls. */
    uint32_t tool_reserved;      /* Since 0.37. Must be zero. */
    const ayni_tool_call_v1 *tool_calls; /* Since 0.37. Null only when tool_call_count is 0. */
} ayni_provider_result_v1;

/* Any non-ISR thread, including synchronous submission. Copies into capacity
 * reserved for the attempt, independently of storage/event queue pressure.
 * A tool list is checked and encoded here: counts, ids, non-singleton values,
 * duplicate fields, invalid UTF-8, or one on an attempt that takes no list,
 * return INVALID_ARGUMENT; a struct larger than this version's returns
 * UNSUPPORTED_VERSION (smaller, INVALID_ARGUMENT); a body over
 * CALL_TEXT_BYTES_MAX, or an encoding over the call's result_text_limit,
 * returns CAPACITY_EXCEEDED. Shape is checked before the attempt is found.
 * Stale/duplicate reliable posts return INVALID_STATE. Older progress is
 * ignored with OK. A successful terminal post ends the delivery borrow;
 * runtime still joins the producer before destroying Core/context storage.
 * Malformed/oversized posts return an error and keep the borrow outstanding.
 */
ayni_status_t ayni_provider_post(ayni_provider_completion_t *completion,
                                 const ayni_provider_result_v1 *result);

/* Since 0.37. One offered tool as the model may use it: the Binding, its
 * capability, the input text limit and each authority field with the values
 * this Task is authorized for. A target or egress field shows only the
 * Binding's own endpoint or destination. Token sets point into tokens.
 * Names and descriptions for the model belong to the Provider's trusted
 * configuration, keyed by binding_id and field_id (ADR-0054). */
typedef struct ayni_tool_v1 {
    uint32_t struct_size;
    uint32_t binding_version;
    ayni_string_v1 binding_id;
    ayni_string_v1 capability_id;
    uint32_t capability_version;
    uint32_t text_limit; /* The Binding's input text limit, in bytes. */
    uint32_t field_count;
    uint32_t reserved; /* Written zero. */
    ayni_authority_field_desc_v1 fields[AYNI_AUTHORITY_FIELD_COUNT_MAX];
    ayni_string_v1 tokens[AYNI_AUTHORITY_FIELD_COUNT_MAX][AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX];
} ayni_tool_v1;

/* Callback inputs borrow Core storage only until the callback returns. A
 * retaining Provider copies them before returning ACCEPTED/ACCEPT_UNKNOWN.
 */
typedef struct ayni_provider_request_v1 {
    uint32_t struct_size;         /* sizeof this structure. */
    uint32_t capability_version;  /* Exact registered version. */
    ayni_attempt_ref_v1 attempt;  /* Nonzero boot/request pair; never reused. */
    uint64_t logical_call_id;     /* Stable across attempts of this step. */
    uint64_t operation_id;        /* Current generation operation; stable for ordinary calls. */
    uint64_t deadline_us;         /* Current boot monotonic deadline. */
    ayni_string_v1 capability_id; /* Matched capability, not model-supplied endpoint. */
    ayni_string_v1 endpoint_id;   /* Trusted local endpoint token; never a URL. */
    ayni_scope_v1 arguments;      /* Complete concrete values actually authorized. */
    ayni_string_v1 text;          /* Bounded UTF-8 business body, never credentials. */
    /* Since 0.35. Borrowed through submit only; null for nonphysical I/O. Physical
     * Providers hand this permit to the gate: a local gate installs it before
     * acceptance returns; since 0.36 a remote one may receive it later, unless a
     * status read answered NOT_FOUND for it first (ADR-0052). Workers only issue
     * samples under that gate. Copy before retention, never accept it later. */
    const ayni_execution_permit_v1 *execution_permit;
    /* Since 0.37. Tools offered to a TOOL_CALLS call, zero otherwise. During
     * this submit only, on its thread, tool(tool_context, index, out) fills
     * caller storage whose struct_size is set, for index below tool_count; its
     * views borrow Core storage until submit returns. Outside any submit it
     * returns INVALID_STATE; a Provider never keeps it, since during a later
     * submit it would describe that call's tools. Null when tool_count is 0. */
    uint32_t tool_count;
    uint32_t tool_reserved; /* Written zero. */
    const void *tool_context;
    ayni_status_t (*tool)(const void *context, uint32_t index, ayni_tool_v1 *out);
    /* Since 0.43. Non-null only for a subscription; copy during submit. */
    const ayni_subscription_open_v1 *subscription;
} ayni_provider_request_v1;

/* All callbacks run on the owner thread, without reentry or blocking I/O.
 * Accepted submit/query borrows completion until exactly one terminal post.
 * Cancel never releases that borrow, even when rejected. Provider shutdown
 * must join producers before the composition root destroys their contexts.
 */
typedef struct ayni_provider_v1 {
    uint32_t struct_size;          /* sizeof this structure. */
    uint32_t version;              /* Positive exact implementation contract version. */
    ayni_string_v1 provider_id;    /* Nonempty catalog identity. */
    ayni_string_v1 endpoint_id;    /* One trusted endpoint per registration. */
    ayni_string_v1 destination_id; /* Egress token; empty iff every offer is local. */
    void *ctx;                     /* Caller-owned, null permitted. */
    uint32_t capability_count;     /* 1..PROVIDER_CAPABILITY_COUNT_MAX. */
    uint32_t reserved;             /* Zero. */
    const ayni_call_capability_v1 *capabilities; /* Copied exact offers, no duplicate ID/version. */
    ayni_submit_result_t (*submit)(void *ctx, const ayni_provider_request_v1 *request,
                                   ayni_provider_completion_t *completion);
    /* Required iff at least one offer declares remote cancellation. */
    ayni_submit_result_t (*cancel)(void *ctx, ayni_attempt_ref_v1 attempt);
    /* Required iff at least one offer declares remote outcome queries. */
    ayni_submit_result_t (*query)(void *ctx, uint64_t operation_id, ayni_attempt_ref_v1 attempt,
                                  ayni_provider_completion_t *completion);
    /* Since 0.43. Required for subscription offers; nonblocking, ACK through
     * the original completion. REJECTED_NOT_RETAINED preserves the old lease. */
    ayni_submit_result_t (*renew)(void *ctx, const ayni_subscription_renew_v1 *request);
} ayni_provider_v1;

/* Explicit immutable binding, checked as a whole before start. Changing a
 * Provider never silently retargets an accepted logical call or operation.
 */
typedef struct ayni_binding_v1 {
    uint32_t struct_size;          /* sizeof this structure. */
    uint32_t version;              /* Positive binding revision, matched exactly. */
    ayni_string_v1 binding_id;     /* Nonempty catalog identity. */
    ayni_string_v1 capability_id;  /* Exact authority/call capability identity. */
    uint32_t capability_version;   /* Positive exact semantic version. */
    uint32_t provider_version;     /* Positive exact Provider version. */
    ayni_string_v1 provider_id;    /* Exact selected Provider identity. */
    ayni_string_v1 endpoint_id;    /* Must equal that Provider's endpoint token. */
    ayni_string_v1 destination_id; /* Must equal its egress token; empty for local. */
} ayni_binding_v1;

/* Trusted owner thread, CONFIGURING only. Descriptors are copied; invalid,
 * duplicate or full registrations change nothing. Cross-references resolve
 * at start so order of successful registration has no meaning.
 */
ayni_status_t ayni_register_call_capability(ayni_kernel_t *kernel,
                                            const ayni_call_capability_v1 *capability);
ayni_status_t ayni_register_provider(ayni_kernel_t *kernel, const ayni_provider_v1 *provider);
ayni_status_t ayni_register_binding(ayni_kernel_t *kernel, const ayni_binding_v1 *binding);

/* Since 0.31. Observable call progress, distinct from Task lifecycle. */
typedef uint32_t ayni_call_phase_t;
#define AYNI_CALL_QUEUED ((ayni_call_phase_t)1) /* Accepted, awaiting resources. Since 0.31. */
#define AYNI_CALL_PREPARED                                                                         \
    ((ayni_call_phase_t)2) /* Resources admitted, preparation recorded. Since 0.31. */
#define AYNI_CALL_DISPATCHING                                                                      \
    ((ayni_call_phase_t)3) /* Durable barrier required before delivery. Since 0.31. */
#define AYNI_CALL_IN_PROGRESS ((ayni_call_phase_t)4) /* Provider reported progress. Since 0.31. */
#define AYNI_CALL_FINISHED                                                                         \
    ((ayni_call_phase_t)5) /* Local tracking ended; inspect outcome/effect. Since 0.31. */
#define AYNI_CALL_RESULT_UNKNOWN                                                                   \
    ((ayni_call_phase_t)6) /* Unresolved remote outcome, never auto-retried. Since 0.31. */

typedef uint32_t ayni_call_wait_t;
#define AYNI_CALL_WAIT_NONE                                                                        \
    ((ayni_call_wait_t)0) /* No external wait, or already terminal. Since 0.31. */
#define AYNI_CALL_WAIT_FIFO                                                                        \
    ((ayni_call_wait_t)1) /* Earlier work of the same Agent. Since 0.31.                           \
                           */
#define AYNI_CALL_WAIT_OCCUPIED                                                                    \
    ((ayni_call_wait_t)2) /* A required resource is occupied. Since 0.31. */
#define AYNI_CALL_WAIT_OFFLINE                                                                     \
    ((ayni_call_wait_t)3) /* A required resource is offline. Since 0.31. */
#define AYNI_CALL_WAIT_FAULTED                                                                     \
    ((ayni_call_wait_t)4) /* A required resource has failed health. Since 0.31. */
#define AYNI_CALL_WAIT_QUARANTINED                                                                 \
    ((ayni_call_wait_t)5) /* Stop confirmation is pending. Since 0.31. */
#define AYNI_CALL_WAIT_DURABILITY                                                                  \
    ((ayni_call_wait_t)6) /* A required journal prefix is not durable. Since 0.31. */
#define AYNI_CALL_WAIT_PROVIDER                                                                    \
    ((ayni_call_wait_t)7) /* Provider still owes a terminal receipt. Since 0.31. */
#define AYNI_CALL_WAIT_COMPATIBILITY                                                               \
    ((ayni_call_wait_t)8) /* Saved contract unavailable this boot. Since 0.31. */

/* Another resource in the configured conflict domain is held. Since 0.32. */
#define AYNI_CALL_WAIT_CONFLICT ((ayni_call_wait_t)9)

/* Since 0.31. Fixed copied diagnostics; no private representation or body pointers. */
typedef uint32_t ayni_call_query_phase_t;
#define AYNI_QUERY_NOT_REQUESTED                                                                   \
    ((ayni_call_query_phase_t)0) /* No owner query accepted. Since 0.31. */
#define AYNI_QUERY_REQUESTED                                                                       \
    ((ayni_call_query_phase_t)1) /* Accepted; awaiting a query attempt. Since 0.31. */
#define AYNI_QUERY_DISPATCHING                                                                     \
    ((ayni_call_query_phase_t)2) /* Query may be in flight; execution is not retried. Since 0.31.  \
                                  */
#define AYNI_QUERY_FINISHED                                                                        \
    ((ayni_call_query_phase_t)3) /* Query tracking ended; inspect conclusion/status. Since 0.31.   \
                                  */

typedef struct ayni_call_info_v1 {
    uint32_t struct_size;
    ayni_call_phase_t phase;
    uint64_t logical_call_id;    /* Stable nonzero call identity. */
    uint64_t task_id;            /* Exact retained owner Task. */
    uint64_t operation_id;       /* Current generation operation; stable for ordinary calls. */
    uint64_t owner_instance_id;  /* Agent instance for resource fairness. */
    ayni_attempt_ref_v1 attempt; /* Both zero until dispatch preparation. */
    uint64_t progress_sequence;  /* Greatest applied progress sequence, zero initially. */
    uint64_t requires_seq;       /* Latest fact's journal sequence; compare Kernel durable_seq. */
    ayni_call_outcome_t outcome; /* Zero pending; otherwise terminal tracking outcome. */
    ayni_status_t status;        /* Classified outcome; no Provider diagnostic string. */
    ayni_reason_t reason;        /* Classified cause. */
    ayni_call_effect_t effect; /* Known NONE before possible delivery, otherwise ledger evidence. */
    uint32_t cancel_requested; /* 1 after a committed request; does not mean stopped. */
    ayni_call_outcome_t cancel_result; /* Zero, CANNOT_CANCEL or CANCEL_UNKNOWN. */
    uint32_t input_bytes;              /* Retained complete canonical input length. */
    uint32_t result_bytes;        /* Retained UTF-8 result length, zero permits empty success. */
    ayni_call_wait_t wait_reason; /* Derived from pending runtime ownership. */
    uint32_t tool_call_count;     /* Since 0.37. Tool calls held instead of result text. */
    uint64_t waited_us;           /* This boot's resource wait duration; zero outside a queue. */
    uint32_t query_revision; /* Starts at one; CAS increases once per accepted management query. */
    ayni_call_query_phase_t query_phase; /* Zero before the first owner query. */
    ayni_status_t query_status;        /* Last query diagnostic, independent of execution status. */
    ayni_reason_t query_reason;        /* Classified query cause; NONE initially. */
    uint32_t query_conclusion;         /* Zero until finished, then QUERY_RESULT_*. */
    uint32_t query_reserved;           /* Written zero. */
    ayni_attempt_ref_v1 query_attempt; /* Distinct from original execution; zero before prepared. */
} ayni_call_info_v1;

/* Owner thread, outside callbacks. Fixed copied info may describe volatile facts;
 * requires_seq identifies their publication barrier. NOT_FOUND preserves output. */
ayni_status_t ayni_get_call(const ayni_kernel_t *kernel, uint64_t logical_call_id,
                            ayni_call_info_v1 *out_info);
/* Copy terminal text to caller storage only after its batch is durable. BUSY for
 * pending/not-yet-durable, RESULT_UNKNOWN for abandoned tracking. BUFFER_TOO_SMALL
 * sets out_size only; other failures preserve outputs. No terminating NUL added.
 * A result of tool calls has no text and copies zero bytes. */
ayni_status_t ayni_copy_call_result(const ayni_kernel_t *kernel, uint64_t logical_call_id,
                                    char *buffer, uint32_t capacity, uint32_t *out_size);

#ifdef __cplusplus
}
#endif
#endif /* AYNI_CALLS_H */
