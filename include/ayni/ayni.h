/*
 * Ayni public C ABI (experimental, ABI 0.x).
 *
 * Only the functions declared in this header are implemented. Planned
 * interfaces live under include/ayni/draft/; they are compiled by the header
 * checks but have no symbols yet. Semantics are specified in contracts/.
 *
 * Rules that apply to every function here:
 * - Kernel functions are called on the kernel owner thread only, unless
 *   their comment names other threads.
 * - A function the kernel calls, such as a host function, a goal type's
 *   validator or a strategy's decide function, must not call kernel
 *   functions unless its comment allows it. As a fail-safe, the owner-thread
 *   functions that take a non-const ayni_kernel_t return AYNI_INVALID_STATE
 *   without effect when called from a validator or a decide function (a
 *   callback), after their argument checks and before their state checks.
 * - Inputs are copied before return unless a parameter says otherwise.
 * - Each function lists every status it can return (C-STATUS-001). A failed
 *   call has no effect and writes no output unless its comment says so.
 * - Every ABI struct starts with struct_size, set to sizeof the caller's view
 *   of the struct (contracts/domain/versions.md). A struct_size below the
 *   first published layout of the ABI major is AYNI_INVALID_ARGUMENT; one
 *   larger than this library knows means the caller was built against a
 *   newer minor and is AYNI_UNSUPPORTED_VERSION. A shorter struct from an
 *   older caller gets defaults for the fields it lacks.
 */
#ifndef AYNI_AYNI_H
#define AYNI_AYNI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Major 0 is experimental: any minor may change published semantics
 * (C-VER-006), so ayni_create accepts only the exact major and minor of this
 * library. From major 1 on, a minor only appends fields or functions and a
 * caller built against an older minor of the same major is accepted.
 */
#define AYNI_ABI_MAJOR 0u
#define AYNI_ABI_MINOR 35u

/*
 * Status codes (contracts/domain/status_codes.md). 0 is success and negative
 * values are failures. A published value is never reused for another meaning;
 * callers treat an unknown negative value as a generic failure.
 */
typedef int32_t ayni_status_t;

#define AYNI_OK ((ayni_status_t)0)
#define AYNI_INVALID_ARGUMENT ((ayni_status_t)(-1))
#define AYNI_INVALID_STATE ((ayni_status_t)(-2))
#define AYNI_UNSUPPORTED_VERSION ((ayni_status_t)(-3))
#define AYNI_UNSUPPORTED ((ayni_status_t)(-4))
#define AYNI_NOT_READY ((ayni_status_t)(-5))
#define AYNI_BUSY ((ayni_status_t)(-6))
#define AYNI_CAPACITY_EXCEEDED ((ayni_status_t)(-7))
#define AYNI_BUFFER_TOO_SMALL ((ayni_status_t)(-8))
#define AYNI_NOT_FOUND ((ayni_status_t)(-9))
#define AYNI_DENIED ((ayni_status_t)(-10))
#define AYNI_EXPIRED ((ayni_status_t)(-11))
#define AYNI_STALE ((ayni_status_t)(-12))
#define AYNI_BUDGET_EXHAUSTED ((ayni_status_t)(-13))
#define AYNI_STORAGE_FAILED ((ayni_status_t)(-14))
#define AYNI_REVISION_CONFLICT ((ayni_status_t)(-15))
#define AYNI_IDEMPOTENCY_CONFLICT ((ayni_status_t)(-16))
#define AYNI_RESULT_UNKNOWN ((ayni_status_t)(-17))
#define AYNI_INTERNAL ((ayni_status_t)(-18))

/* Kernel lifecycle (contracts/domain/lifecycles.md, plan section 3.2). */
typedef uint32_t ayni_lifecycle_t;

#define AYNI_LIFECYCLE_CONFIGURING ((ayni_lifecycle_t)1)
#define AYNI_LIFECYCLE_RECOVERING ((ayni_lifecycle_t)2)
#define AYNI_LIFECYCLE_RUNNING ((ayni_lifecycle_t)3)
#define AYNI_LIFECYCLE_RESTRICTED ((ayni_lifecycle_t)4)
#define AYNI_LIFECYCLE_DRAINING ((ayni_lifecycle_t)5)
#define AYNI_LIFECYCLE_STOPPED ((ayni_lifecycle_t)6)
/* Since 0.15. Writer isolation is pending; no new work is admitted. */
#define AYNI_LIFECYCLE_FENCED ((ayni_lifecycle_t)7)

typedef struct ayni_kernel ayni_kernel_t;

typedef struct ayni_config_v1 {
    uint32_t struct_size;
    uint32_t abi_major; /* must equal AYNI_ABI_MAJOR */
    /* Since 0.2; must equal AYNI_ABI_MINOR while the major is 0. A caller
     * whose struct_size ends before this field was built against 0.1. */
    uint32_t abi_minor;
    /*
     * Since 0.3. Capacities fixed for the kernel's lifetime (ADR-0014,
     * contracts/domain/limits.md). Each must be between its _MIN (1 unless
     * defined) and its _MAX; a field that struct_size does not cover takes its
     * _DEFAULT. The defaults are unmeasured initial assumptions (C-LIMIT-003).
     */
    uint32_t ingress_capacity; /* events queued by ayni_post_event */
    /* Room for an event body in each queued event. No implemented event kind
     * carries a body yet; since 0.9 each queued event keeps at least
     * AYNI_CATALOG_ID_MAX_BYTES for a local input's source (ADR-0021). */
    uint32_t event_body_max_bytes;
    uint32_t completion_slot_count; /* asynchronous Host requests in flight */
    /* Since 0.4. The buffer for log reads during recovery (ADR-0016) and,
     * since 0.14, for the batches committed in RUNNING until the host has
     * stored them (ADR-0026). Each transition needs room for the largest
     * batch the other capacities allow before it starts, so the buffer must
     * hold at least that; since 0.15, 0 takes three times that. A larger
     * buffer lets more batches wait for the host. */
    uint32_t storage_buffer_bytes;
    /* Since 0.5. The work model (ADR-0017). The last two are limits rather
     * than storage, with the same range and default rules. Since 0.13 a
     * terminal goal gives its place to a new one once goal_capacity goals
     * exist (ADR-0025). */
    uint32_t agent_capacity;           /* registered agents, one instance each */
    uint32_t goal_type_capacity;       /* registered goal type versions */
    uint32_t goal_capacity;            /* goals in memory, terminal ones included */
    uint32_t goal_parameter_max_bytes; /* largest goal parameter block */
    uint32_t session_capacity;         /* sessions open at once */
    uint32_t goal_depth_max;           /* deepest child goal; a top-level goal has depth 0 */
    uint32_t goal_children_max;        /* children per goal that are not yet terminal */
    /* Since 0.6 (ADR-0018). Tasks in memory; an ended task gives its place to
     * a new one when the table is full. */
    uint32_t task_capacity;
    /* Since 0.10 (ADR-0022). */
    uint32_t strategy_capacity; /* registered strategies */
    /* The intents one decision may hold; each strategy declares its own
     * max_intents_per_call up to this. */
    uint32_t policy_intent_capacity;
    /* Since 0.12 (ADR-0024). The steps one task's decisions may commit. */
    uint32_t task_step_capacity;
    /* Room for one task's private checkpoint; each strategy declares its
     * own max_checkpoint_bytes up to this. */
    uint32_t task_checkpoint_max_bytes;
    /* Since 0.14 (ADR-0026). Accepted commands each principal keeps mapped
     * to their results (ayni_intent_v1 command_id). */
    uint32_t command_capacity;
    /* Since 0.15. Minimum time between persistent requests, including
     * retries and maintenance. 0 selects 10 ms; reads are not limited. */
    uint32_t storage_write_interval_us;
    /* Since 0.15. Encoded log byte limit. 0 selects the larger of 1 MiB
     * and eight storage buffers. Maintenance reserves space before admit. */
    uint32_t storage_log_max_bytes;
    /* Since 0.17. Capability authorization schemas and their canonical
     * domain/scope byte limit. Explicit zero is invalid; omitted fields
     * take the documented defaults. Registration does not enable calls. */
    uint32_t capability_capacity;
    uint32_t authority_scope_max_bytes;
    /* Since 0.18. Grants retain a source domain and scope each; depth
     * counts delegation edges, with root depth 0. Explicit zero is invalid. */
    uint32_t grant_capacity;
    uint32_t grant_delegation_depth_max;
    /* Since 0.20. Requirements per Goal: 1..16; an omitted trailing field uses 4.
     * Each original domain and Scope separately uses authority_scope_max_bytes. */
    uint32_t goal_requirement_capacity;
    /* Since 0.29. Resource descriptors/live state and queued/held resource
     * requests. Zero (also the omitted default) disables each table. An
     * explicit profile enables them; no implicit hardware/resource discovery. */
    uint32_t resource_capacity;
    uint32_t resource_request_capacity;
    /* Since 0.30. Per-Task model call hard ceiling, 1..64; 0 or omitted
     * selects 64. Initial soft limit is min(8, ceiling). Fixed for this boot;
     * independent of concurrent Provider slots and committed step capacity. */
    uint32_t task_model_call_hard_limit;
    /* Since 0.31. Omitted/zero disables the respective trusted directory.
     * Counts include exact versions; none grows during execution. */
    uint32_t call_capability_capacity;
    uint32_t provider_capacity;
    uint32_t binding_capacity;
    /* Since 0.31. Retained calls and separate Provider delivery slots; zero
     * disables calls. Bytes are fixed per call; zero selects the defaults. */
    uint32_t call_capacity;
    uint32_t call_input_max_bytes;
    uint32_t call_result_max_bytes;
    /* Since 0.32. Trusted physical topology; zero disables each directory. */
    uint32_t device_capacity;
    uint32_t component_capacity;
    /* Since 0.35. Copied source declarations and latest observations; zero disables. */
    uint32_t observation_source_capacity;
    /* Since 0.35. Copied Scene declarations and persistent preferences; zero disables. */
    uint32_t scene_capacity;
    /* Since 0.35. Local executor registrations and boot-local permit slots; zero disables. */
    uint32_t executor_capacity;
    /* Since 0.35. Exact physical Binding constraints; zero disables. */
    uint32_t execution_binding_capacity;
} ayni_config_v1;

/* Since 0.31. Bounded retained call bodies; no implicit history eviction. */
#define AYNI_CALL_CAPACITY_MAX 256u
#define AYNI_CALL_INPUT_BYTES_DEFAULT 4096u
#define AYNI_CALL_INPUT_BYTES_MAX 131072u
#define AYNI_CALL_RESULT_BYTES_DEFAULT 4096u
#define AYNI_CALL_RESULT_BYTES_MAX 65536u

/* Since 0.30. Model request counts, not token or monetary allowances. */
#define AYNI_TASK_MODEL_CALL_LIMIT_DEFAULT 8u
#define AYNI_TASK_MODEL_CALL_HARD_LIMIT_DEFAULT 64u
#define AYNI_TASK_MODEL_CALL_HARD_LIMIT_MAX 64u

/* Since 0.29. Counts of immutable resources and pending/held request groups. */
#define AYNI_RESOURCE_CAPACITY_DEFAULT 0u
#define AYNI_RESOURCE_CAPACITY_MAX 64u
#define AYNI_RESOURCE_REQUEST_CAPACITY_DEFAULT 0u
#define AYNI_RESOURCE_REQUEST_CAPACITY_MAX 256u

/* Since 0.17. Limits apply to complete registered domains and scopes. */
/* Since 0.18. Caller-owned Grant storage and bounded parent traversal. */
#define AYNI_GRANT_CAPACITY_DEFAULT 32u
#define AYNI_GRANT_CAPACITY_MAX 1024u
#define AYNI_GRANT_DELEGATION_DEPTH_MAX_DEFAULT 4u
#define AYNI_GRANT_DELEGATION_DEPTH_MAX_MIN 1u
#define AYNI_GRANT_DELEGATION_DEPTH_MAX_MAX 16u

/* Since 0.20. Bounded Goal authority metadata; counts of requirements. */
#define AYNI_GOAL_REQUIREMENT_CAPACITY_DEFAULT 4u
#define AYNI_GOAL_REQUIREMENT_CAPACITY_MAX 16u
#define AYNI_CAPABILITY_CAPACITY_DEFAULT 16u
#define AYNI_CAPABILITY_CAPACITY_MAX 256u
#define AYNI_AUTHORITY_SCOPE_MAX_BYTES_DEFAULT 256u
#define AYNI_AUTHORITY_SCOPE_MAX_BYTES_MIN 64u
#define AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX 4096u
#define AYNI_INGRESS_CAPACITY_DEFAULT 128u
#define AYNI_INGRESS_CAPACITY_MAX 4096u
#define AYNI_EVENT_BODY_MAX_BYTES_DEFAULT 128u
#define AYNI_EVENT_BODY_MAX_BYTES_MAX 4096u
#define AYNI_COMPLETION_SLOT_COUNT_DEFAULT 16u
#define AYNI_COMPLETION_SLOT_COUNT_MAX 1024u
/* Since 0.15 the default 0 selects three times the maximum batch plus ring
 * overhead. An explicit buffer must hold that batch and any snapshot record. */
#define AYNI_STORAGE_BUFFER_BYTES_DEFAULT 0u
#define AYNI_STORAGE_BUFFER_BYTES_MIN 256u
#define AYNI_STORAGE_BUFFER_BYTES_MAX 1048576u
#define AYNI_AGENT_CAPACITY_DEFAULT 4u
#define AYNI_AGENT_CAPACITY_MAX 64u
#define AYNI_GOAL_TYPE_CAPACITY_DEFAULT 8u
#define AYNI_GOAL_TYPE_CAPACITY_MAX 64u
#define AYNI_GOAL_CAPACITY_DEFAULT 32u
#define AYNI_GOAL_CAPACITY_MAX 1024u
#define AYNI_GOAL_PARAMETER_MAX_BYTES_DEFAULT 128u
#define AYNI_GOAL_PARAMETER_MAX_BYTES_MIN 0u
#define AYNI_GOAL_PARAMETER_MAX_BYTES_MAX 4096u
#define AYNI_SESSION_CAPACITY_DEFAULT 4u
#define AYNI_SESSION_CAPACITY_MAX 64u
#define AYNI_GOAL_DEPTH_MAX_DEFAULT 2u
#define AYNI_GOAL_DEPTH_MAX_MIN 0u
#define AYNI_GOAL_DEPTH_MAX_MAX 8u
#define AYNI_GOAL_CHILDREN_MAX_DEFAULT 4u
#define AYNI_GOAL_CHILDREN_MAX_MIN 0u
#define AYNI_GOAL_CHILDREN_MAX_MAX 64u
#define AYNI_TASK_CAPACITY_DEFAULT 16u
#define AYNI_TASK_CAPACITY_MAX 256u
#define AYNI_STRATEGY_CAPACITY_DEFAULT 4u
#define AYNI_STRATEGY_CAPACITY_MAX 64u
#define AYNI_POLICY_INTENT_CAPACITY_DEFAULT 8u
#define AYNI_POLICY_INTENT_CAPACITY_MAX 64u
#define AYNI_TASK_STEP_CAPACITY_DEFAULT 8u
#define AYNI_TASK_STEP_CAPACITY_MAX 64u
#define AYNI_TASK_CHECKPOINT_MAX_BYTES_DEFAULT 64u
#define AYNI_TASK_CHECKPOINT_MAX_BYTES_MIN 0u
#define AYNI_TASK_CHECKPOINT_MAX_BYTES_MAX 4096u
#define AYNI_COMMAND_CAPACITY_DEFAULT 32u
#define AYNI_COMMAND_CAPACITY_MAX 1024u
#define AYNI_STORAGE_WRITE_INTERVAL_US_DEFAULT 10000u
#define AYNI_STORAGE_WRITE_INTERVAL_US_MAX 1000000u
#define AYNI_STORAGE_LOG_MAX_BYTES_DEFAULT 0u
#define AYNI_STORAGE_LOG_MAX_BYTES_MAX 67108864u

/*
 * Correlates an asynchronous attempt with its result (C-ID-006). Unique
 * within one kernel instance only: request_id restarts with each ayni_create,
 * and every recovery request uses boot_epoch 0. A host posts a result only
 * through the completion it was given with the attempt, and never posts the
 * result of a request made by an earlier kernel instance, such as one
 * outstanding when the previous process crashed.
 */
typedef struct ayni_attempt_ref_v1 {
    uint64_t boot_epoch; /* 0 for requests made before this boot's epoch is durable */
    uint64_t request_id;
} ayni_attempt_ref_v1;

/*
 * Synchronous answer of every asynchronous port (C-HOST-004). Only
 * REJECTED_NOT_RETAINED proves that no work exists; ACCEPTED and
 * ACCEPT_UNKNOWN are both in flight, including work queued inside the host,
 * and each owes exactly one completion post. Any other value is treated as
 * ACCEPT_UNKNOWN and counted as a host defect.
 */
typedef uint32_t ayni_submit_result_t;

#define AYNI_SUBMIT_REJECTED_NOT_RETAINED ((ayni_submit_result_t)1)
#define AYNI_SUBMIT_ACCEPTED ((ayni_submit_result_t)2)
#define AYNI_SUBMIT_ACCEPT_UNKNOWN ((ayni_submit_result_t)3)

/*
 * A completion slot that the kernel reserved before calling an asynchronous
 * host port (C-HOST-006). Kernel-owned; the host keeps the pointer from the
 * accepted call until its single post, and never afterwards (C-HOST-018).
 */
typedef struct ayni_completion ayni_completion_t;

/* Result of one asynchronous host request. */
typedef struct ayni_completion_result_v1 {
    uint32_t struct_size;
    /* Defined per port. 0 is never defined: a post with outcome 0 still
     * completes the slot, is counted as a host defect and is handled as an
     * unknown result. */
    uint32_t outcome;
    ayni_attempt_ref_v1 attempt; /* as passed to the port call */
    uint64_t value;              /* defined per port, e.g. bytes read; 0 otherwise */
    /* Since 0.15. Opaque next physical cursor for storage_read_log.
     * Valid until log rotation. Within one log instance, a nonempty read
     * returns a cursor strictly greater than its input. 0 begins a read;
     * after rotation restart at 0. Other ports write 0. */
    uint64_t next_cursor;
} ayni_completion_result_v1;

/* Since 0.4. A byte view; data may be NULL only when size is 0. */
typedef struct ayni_bytes_v1 {
    const uint8_t *data;
    uint32_t size;
} ayni_bytes_v1;

/*
 * Since 0.4. One commit batch handed to storage_append, encoded as in
 * contracts/storage/records.md. The struct is copied during the call;
 * encoded.data stays kernel-owned and unchanged until the completion for the
 * attempt is posted, and the host may read it from any thread until then
 * (C-HOST-019).
 */
typedef struct ayni_storage_batch_v1 {
    uint32_t struct_size;
    uint32_t writer_epoch;
    uint64_t transition_id;
    uint64_t commit_seq;
    ayni_bytes_v1 encoded; /* header, records and commit marker */
} ayni_storage_batch_v1;

/*
 * Since 0.4. ayni_completion_result_v1.outcome of the storage ports. A write
 * is OK only once the batch survives power loss (C-HOST-010). A read is OK
 * when the buffer holds what the log contains, and reports the bytes read in
 * value; FAILED or UNKNOWN when its content is not reliable. Distinct from
 * the AYNI_STORAGE_FAILED status code, which reports a failed kernel call.
 */
#define AYNI_WRITE_OK 1u
#define AYNI_WRITE_FAILED 2u
#define AYNI_WRITE_UNKNOWN 3u
/* Since 0.15. Read only: value is the exact required first batch size,
 * greater than the supplied buffer; no bytes or cursor were consumed. */
#define AYNI_READ_BUFFER_TOO_SMALL 4u
/* Since 0.15. Fence success returns its actual new writer epoch in value. */
#define AYNI_FENCE_DRAINED 1u
#define AYNI_FENCE_EPOCH_INSTALLED 2u
#define AYNI_FENCE_UNAVAILABLE 3u

/*
 * Platform services the kernel uses to maintain its own invariants (ADR-0006).
 * ayni_create copies the table; ctx and the functions it points to stay owned
 * by the caller and must remain valid until ayni_destroy returns AYNI_OK.
 * Further services are appended as they are implemented; their planned shape
 * is in include/ayni/draft/host_v1.h.
 */
typedef struct ayni_host_v1 {
    uint32_t struct_size;
    void *ctx;
    /*
     * Required. Monotonic time in microseconds from an arbitrary per-boot
     * origin. Never decreases within a boot; not comparable across boots or
     * devices. Called on the kernel owner thread; must not block.
     */
    uint64_t (*monotonic_now_us)(void *ctx);
    /*
     * Since 0.3. Required. Asks the owner thread to call the kernel soon;
     * several wakes may merge into one. Called from any thread except an ISR,
     * outside the ingress critical section; reentrant and idempotent. Must
     * not block or call into the kernel (C-HOST-007).
     */
    void (*wake)(void *ctx);
    /*
     * Since 0.3. Required. The ingress critical section (ADR-0015): mutual
     * exclusion between the owner thread and the threads that post events
     * and completions. Called from any thread except an ISR; not reentrant.
     * The kernel holds it for one bounded step only: one event queue
     * operation, which reads each queued entry at most once and moves fewer
     * than ingress_capacity indices, or one completion slot change. It calls
     * no other host function while holding it (C-HOST-020).
     */
    void (*ingress_lock)(void *ctx);
    void (*ingress_unlock)(void *ctx);
    /*
     * Since 0.4. Persistent log (C-HOST-009, ADR-0016): both or neither. A
     * host without them can create a kernel, but ayni_start returns
     * AYNI_UNSUPPORTED. Called on the kernel owner thread; must not block.
     * Each one stores or reads whole batches and answers once through the
     * completion it is given, as ayni_submit_result_t describes.
     *
     * storage_append stores one batch after all earlier ones; a batch is
     * either readable whole or not at all, even after power loss mid-write.
     * The host drops a trailing batch it cannot prove complete, for example
     * by its own checksum or a flag written after the data; a length prefix
     * alone proves nothing. The kernel treats every batch it reads as
     * committed, so a damaged one makes it RESTRICTED (ADR-0016).
     *
     * storage_read_prefix copies into buffer the stored batches whose
     * commit_seq is at least from_commit_seq, in log order and only whole
     * batches, as many as fit. The completion value is the number of bytes
     * written; 0 means no batch is left. Since 0.15 READ_BUFFER_TOO_SMALL
     * reports the exact first batch size when it cannot fit. The host may write the buffer only
     * until it posts.
     */
    ayni_submit_result_t (*storage_append)(void *ctx, const ayni_storage_batch_v1 *batch,
                                           ayni_attempt_ref_v1 attempt,
                                           ayni_completion_t *completion);
    ayni_submit_result_t (*storage_read_prefix)(void *ctx, uint64_t from_commit_seq,
                                                uint8_t *buffer, uint32_t buffer_size,
                                                ayni_attempt_ref_v1 attempt,
                                                ayni_completion_t *completion);
    /* Since 0.15. Physical-order log reading. cursor 0 begins at the first
     * frame. OK reports bytes in value and the next cursor in next_cursor;
     * zero bytes is EOF. Whole batches only, including stale writer epochs:
     * the Core, not the Host, owns the fence/replay rules. */
    ayni_submit_result_t (*storage_read_log)(void *ctx, uint64_t cursor, uint8_t *buffer,
                                             uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                             ayni_completion_t *completion);
    /* Isolates every older writer for all mutating storage ports. A success
     * allocates an epoch >= minimum_epoch and strictly above every epoch
     * previously installed by this Host. Isolation does not return borrowed
     * buffers: each accepted attempt still owes its completion. */
    ayni_submit_result_t (*storage_fence_writers)(void *ctx, uint32_t minimum_epoch,
                                                  ayni_attempt_ref_v1 attempt,
                                                  ayni_completion_t *completion);
    /* Writes one immutable chunk. offset 0 resets an inactive slot; later
     * chunks are sequential. Repeating identical bytes at the same offset
     * is idempotent. OK means power-loss durable. Slots are 0 and 1. */
    ayni_submit_result_t (*storage_write_snapshot)(void *ctx, uint32_t writer_epoch, uint32_t slot,
                                                   uint64_t offset, ayni_bytes_v1 bytes,
                                                   ayni_attempt_ref_v1 attempt,
                                                   ayni_completion_t *completion);
    /* Reads up to buffer_size bytes from a slot, including an uncommitted
     * candidate for read-back verification. OK value is bytes copied,
     * zero at EOF or for an absent slot. */
    ayni_submit_result_t (*storage_read_snapshot)(void *ctx, uint32_t slot, uint64_t offset,
                                                  uint8_t *buffer, uint32_t buffer_size,
                                                  ayni_attempt_ref_v1 attempt,
                                                  ayni_completion_t *completion);
    /* Atomically retains the named activation batch and every physical
     * frame after it. Repeating a completed trim is idempotent. The matching
     * batch has writer_epoch, transition_id and commit_seq as given. On OK
     * old physical cursors are invalid; restart reads with cursor 0. */
    ayni_submit_result_t (*storage_trim_prefix)(void *ctx, uint32_t writer_epoch,
                                                uint64_t transition_id, uint64_t commit_seq,
                                                ayni_attempt_ref_v1 attempt,
                                                ayni_completion_t *completion);
    /* Nonblocking cached usable space, net of pending reservations and
     * Host quotas. AYNI_STORAGE_FAILED means space cannot be established. */
    ayni_status_t (*storage_free_bytes)(void *ctx, uint64_t *out_bytes);
} ayni_host_v1;

/*
 * Since 0.4. Why a kernel is RESTRICTED (ADR-0016); NONE in every other
 * state. The kernel keeps only read-only diagnostics and shutdown.
 */
typedef uint32_t ayni_restriction_t;

#define AYNI_RESTRICTION_NONE ((ayni_restriction_t)0)
#define AYNI_RESTRICTION_STORAGE_REJECTED ((ayni_restriction_t)1)      /* host refused a request */
#define AYNI_RESTRICTION_STORAGE_READ_FAILED ((ayni_restriction_t)2)   /* log read not reliable */
#define AYNI_RESTRICTION_STORAGE_WRITE_FAILED ((ayni_restriction_t)3)  /* a batch not stored */
#define AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN ((ayni_restriction_t)4) /* may be stored */
#define AYNI_RESTRICTION_LOG_CORRUPT ((ayni_restriction_t)5)           /* C-STORE-009 */
#define AYNI_RESTRICTION_LOG_UNSUPPORTED ((ayni_restriction_t)6)       /* C-STORE-004 */
#define AYNI_RESTRICTION_BOOT_EPOCH_EXHAUSTED ((ayni_restriction_t)7)
/* Since 0.14 (ADR-0026). The log holds more state than this kernel's
 * capacities and limits do: it was written under larger ones. Since 0.15
 * an explicit READ_BUFFER_TOO_SMALL from the Host has this cause too. */
#define AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY ((ayni_restriction_t)8)
/* Since 0.14. A defect in this library: a transition's batch did not fit
 * the room it was given. */
#define AYNI_RESTRICTION_INTERNAL ((ayni_restriction_t)9)
/* Since 0.15. Counters never wrap, and space failure never drops a barrier. */
#define AYNI_RESTRICTION_WRITER_EPOCH_EXHAUSTED ((ayni_restriction_t)10)
#define AYNI_RESTRICTION_STORAGE_FULL ((ayni_restriction_t)11)
#define AYNI_RESTRICTION_GENERATION_EXHAUSTED ((ayni_restriction_t)12)
/* Since 0.16. The authenticated Host binding differs from stored ownership. */
#define AYNI_RESTRICTION_OWNER_BINDING_MISMATCH ((ayni_restriction_t)13)
/* Request identity space exhausted; no further I/O may be issued. Since 0.31. */
#define AYNI_RESTRICTION_ATTEMPT_ID_EXHAUSTED ((ayni_restriction_t)14)

/* Since 0.4. Kernel-wide state for diagnostics. */
typedef struct ayni_kernel_info_v1 {
    uint32_t struct_size;
    ayni_lifecycle_t lifecycle;
    ayni_restriction_t restriction;
    uint32_t reserved;   /* written as 0 */
    uint64_t boot_epoch; /* 0 until this boot's epoch is durable (C-ID-009) */
    /*
     * Since 0.5. Every transition with a commit_seq up to this one survives
     * power loss (C-COMMIT-004). During recovery it is the end of the log
     * read so far; from RUNNING on, the batch that stored the boot epoch,
     * and since 0.14 the last batch of a transition the host has stored
     * (ADR-0026). Since 0.15, while the reload after FENCED reads the log
     * again, it stays at least at its value when FENCED began. Compare a
     * commit_seq with it only within the boot that assigned that
     * commit_seq: a later boot assigns the positions above durable_seq
     * again (C-COMMIT-016).
     */
    uint64_t durable_seq;
    /* Since 0.9 (ADR-0021). Events ayni_step took in RUNNING that no goal's
     * trigger took, and events it discarded in DRAINING, RESTRICTED or
     * FENCED (since 0.15), or left queued on entering STOPPED. */
    uint64_t events_unmatched;
    uint64_t events_discarded;
} ayni_kernel_info_v1;

/* Since 0.4. What one ayni_step did. */
typedef struct ayni_step_report_v1 {
    uint32_t struct_size;
    uint32_t items_done;
    uint32_t more_ready; /* nonzero: call ayni_step again without waiting for a wake */
    uint32_t reserved;   /* written as 0 */
    /* Host monotonic time by which to call ayni_step even if nothing wakes
     * the owner; UINT64_MAX: none. Since 0.7, in RUNNING, when the goals'
     * next due work comes (ADR-0019, ADR-0020), or the step's start when
     * some already has and more_ready is set. That is the earliest of the
     * goals' expiries and either the next occurrence of a goal whose tasks
     * may advance, while the task table has room and an id is left, or else
     * the end of such an occurrence's wait. Since 0.14 none while the
     * storage buffer has no room for another transition: the post of the
     * append that frees it wakes the owner. Since 0.15 also includes
     * storage write intervals and retry backoff while new work is frozen;
     * an accepted request waits for its completion wake without a timer. */
    uint64_t next_deadline_us;
    /* Since 0.14. ayni_kernel_info_v1 durable_seq when the step returned. */
    uint64_t durable_seq;
} ayni_step_report_v1;

/*
 * Writes the ABI version this library implements. Callable from any thread.
 * Returns AYNI_OK, or AYNI_INVALID_ARGUMENT if an output pointer is NULL.
 */
ayni_status_t ayni_get_abi_version(uint32_t *out_major, uint32_t *out_minor);

/*
 * Reports the size and alignment of the memory block that ayni_create needs
 * for this configuration. Core never allocates memory itself (ADR-0014).
 * *out_align is a power of two. Returns AYNI_OK; AYNI_INVALID_ARGUMENT if
 * an output pointer is NULL; otherwise AYNI_INVALID_ARGUMENT or
 * AYNI_UNSUPPORTED_VERSION exactly as ayni_create would for the same
 * configuration.
 */
ayni_status_t ayni_kernel_memory_requirements(const ayni_config_v1 *config, size_t *out_size,
                                              size_t *out_align);

/*
 * Creates a kernel in AYNI_LIFECYCLE_CONFIGURING inside caller-provided
 * memory. config and the host table are copied before return. The memory
 * block stays owned by the caller, who must neither use nor release it until
 * ayni_destroy returns AYNI_OK.
 *
 * Failures, with *out_kernel set to NULL (when out_kernel is not NULL) and
 * the memory left untouched:
 * - AYNI_INVALID_ARGUMENT: a null pointer, a struct_size shorter than the
 *   first published layout, a capacity outside its range, a missing required
 *   host function, an incomplete storage port group, or memory not aligned
 *   as reported by ayni_kernel_memory_requirements;
 * - AYNI_UNSUPPORTED_VERSION: the caller's ABI version is not accepted (see
 *   AYNI_ABI_MAJOR), or a struct_size is larger than this library knows;
 * - AYNI_BUFFER_TOO_SMALL: memory_size is below the reported size.
 */
ayni_status_t ayni_create(const ayni_config_v1 *config, const ayni_host_v1 *host, void *memory,
                          size_t memory_size, ayni_kernel_t **out_kernel);

/*
 * Writes the current lifecycle state. Returns AYNI_OK, or
 * AYNI_INVALID_ARGUMENT if kernel or out_state is NULL.
 */
ayni_status_t ayni_get_lifecycle(const ayni_kernel_t *kernel, ayni_lifecycle_t *out_state);

/*
 * Writes the fields of out_info that out_info->struct_size covers whole; a
 * field it covers in part is left as it was. Callable in every lifecycle
 * state. Returns AYNI_OK; AYNI_INVALID_ARGUMENT for a NULL
 * pointer; AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for
 * out_info->struct_size as for every ABI struct.
 */
ayni_status_t ayni_get_kernel_info(const ayni_kernel_t *kernel, ayni_kernel_info_v1 *out_info);

/*
 * Starts recovery: CONFIGURING -> RECOVERING (ADR-0016). Recovery then runs
 * in ayni_step: it reads the log, persists this boot's epoch in its own batch
 * and only then enters RUNNING (C-ID-009), or enters RESTRICTED with a reason
 * (ayni_get_kernel_info). Returns AYNI_OK; AYNI_INVALID_ARGUMENT if kernel is
 * NULL; AYNI_INVALID_STATE outside CONFIGURING; AYNI_UNSUPPORTED, staying in
 * CONFIGURING, when the host lacks the complete storage port group
 * (including a legacy table with only append/read_prefix); since
 * 0.10 AYNI_NOT_FOUND, staying in CONFIGURING, when a goal type names a
 * strategy that is not registered (C-LIFE-003).
 */
ayni_status_t ayni_start(ayni_kernel_t *kernel);

/*
 * Applies posted completions and advances recovery and shutdown. In
 * RUNNING it then does the goals' due work, one item and transition each:
 * expires goals (ADR-0020), derives tasks (ADR-0018) and skips occurrences
 * that waited too long (ADR-0020). A task stands for every occurrence of
 * its goal due by the step's start (ADR-0019). Deriving and skipping set
 * the goal's occurrences, next_due_us, last_task_id or occurrences_skipped
 * and skip_reason, and commit_seq but keep its revision, so the revision a
 * caller holds for the goal stays valid. An expiry that would raise a
 * revision past the largest is dropped instead, the same way: the goal
 * keeps its lifecycle and never expires (C-COMMIT-012). Since 0.11 a skip
 * that leaves the goal with no occurrence to come and no task that has not
 * ended also makes it FAILED, as ayni_trigger_kind_t says (ADR-0023).
 * Since 0.10 it then has the strategies decide the tasks that were READY
 * when the step began, one item and transition each, the task READY
 * longest first (ADR-0022, ayni_register_strategy); since 0.12 a task a
 * decision leaves READY waits for a later step (ADR-0024). Since 0.9 it
 * then takes the events queued when it comes to them, one item each
 * (ADR-0021):
 * in RUNNING each is recorded, in one transition, as an occurrence of every
 * goal whose trigger takes it, due at the step's start, or counted as
 * unmatched; in DRAINING, RESTRICTED and FENCED each is discarded and counted; in
 * RECOVERING they stay queued. Does at most max_items items and stops once
 * max_time_us has passed on the host clock, checked after each item; never
 * waits on storage or any other I/O (C-LIMIT-011). out_report (nullable)
 * receives the fields its struct_size covers.
 *
 * Since 0.14 every transition, in RUNNING, writes one batch into the
 * storage buffer and waits there until the host has stored it (ADR-0026):
 * a step ends by starting the append of the oldest batch waiting when no
 * append is in flight, whatever is left of max_items and max_time_us; that
 * call is not an item, and applying its completion is one. When the buffer has no room for another
 * transition, the due work, decisions and events wait for the appends in flight, and more_ready
 * leaves them out. Since 0.15 failures and unknown results retry the same batch
 * at most three times, with 10/50 ms backoff; only a failed attempt freezes
 * new transitions. Exhaustion enters FENCED, isolates the old writer and
 * reloads the durable state under a new boot epoch, or stays RESTRICTED if
 * isolation or recovery fails. Automatic snapshots drain the log, publish
 * an inactive slot and activate it before trimming covered history. These
 * maintenance stages also pause new work and report storage deadlines.
 * Recovery replays the log into the work model before it enters RUNNING: instances, goals,
 * tasks with their steps and checkpoints, and command mappings come back as the last stored
 * transition left them, while sessions do not. Every wait on the host clock starts again in full
 * from the end of recovery. A log that holds more than this kernel's capacities and limits makes it
 * RESTRICTED with AYNI_RESTRICTION_STATE_EXCEEDS_CAPACITY, and a log recovery cannot replay leaves
 * nothing of it in memory. Since 0.16 a stored owner binding must match
 * the Host configuration before any new recovery write; a mismatch enters
 * RESTRICTED and preserves the validated model for diagnosis. Since 0.27,
 * compatible schema 2 work with complete valid authority continues normally;
 * user/policy/compatibility holds and independent authorization denies remain.
 * Old-boot timed Grants expire without reanchoring. Existing owner-controlled
 * work or commands without a saved binding are refused before new writes;
 * an ownerless store with only Agent instances may still bind its first owner.
 * Entering RUNNING is one transition when it first binds an owner, starts
 * an instance, or settles restored work against its goal and owner.
 *
 * Returns AYNI_OK;
 * AYNI_INVALID_ARGUMENT if kernel is NULL or max_items or max_time_us is 0;
 * AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for
 * out_report->struct_size; AYNI_INVALID_STATE in CONFIGURING and STOPPED,
 * and when called from a callback.
 */
ayni_status_t ayni_step(ayni_kernel_t *kernel, uint32_t max_items, uint32_t max_time_us,
                        ayni_step_report_v1 *out_report);

/*
 * Since 0.15 also accepts FENCED. Enters DRAINING from RECOVERING, RUNNING
 * or RESTRICTED; no new storage
 * request is started after that, except, since 0.14, the appends of the
 * batches RUNNING committed (ADR-0026). It does not wake the owner: the
 * caller goes on calling ayni_step, which enters STOPPED once every reserved
 * completion has been posted and applied (C-HOST-018) and those batches are
 * stored or the bounded append retries failed. Borrowed storage buffers
 * are never released merely because a request times out. Returns
 * AYNI_OK, also in DRAINING and STOPPED; AYNI_INVALID_ARGUMENT if kernel is
 * NULL; AYNI_INVALID_STATE in CONFIGURING, where ayni_destroy applies
 * directly, and when called from a callback.
 */
ayni_status_t ayni_begin_shutdown(ayni_kernel_t *kernel);

/*
 * Posts the single result of a reserved completion, then calls wake. Callable
 * from any thread except an ISR; never fails for lack of space, because the
 * slot was reserved before the request was issued. Must not be called once
 * ayni_destroy has been entered (C-HOST-018). The result is copied.
 * Returns AYNI_OK; AYNI_INVALID_ARGUMENT for a NULL pointer or a readable
 * ayni_completion_t that is not one of a kernel's slots, such as a copy (any
 * other pointer is undefined behavior); AYNI_INVALID_ARGUMENT or
 * AYNI_UNSUPPORTED_VERSION for result->struct_size as for every ABI struct;
 * AYNI_INVALID_STATE, with no effect except a diagnostic count, when the slot
 * is not waiting for this attempt: a second post, or a post for an attempt
 * the slot no longer serves (C-HOST-006).
 */
ayni_status_t ayni_completion_post(ayni_completion_t *completion,
                                   const ayni_completion_result_v1 *result);

/*
 * Ends the kernel's use of its memory and of the host functions and ctx.
 * Returns AYNI_OK; AYNI_INVALID_ARGUMENT if kernel is NULL; AYNI_INVALID_STATE
 * outside CONFIGURING and STOPPED, leaving the kernel unchanged. After
 * AYNI_OK the handle is invalid and the caller may release the memory block
 * and ctx.
 */
ayni_status_t ayni_destroy(ayni_kernel_t *kernel);

/* ---- Work model (since 0.5) --------------------------------------------
 *
 * Principals, sessions, agents, goal types, goals and tasks (ADR-0017,
 * ADR-0018, contracts/domain/primitives.md and lifecycles.md). An intent is
 * applied in memory before ayni_submit_intent returns and is given a
 * commit_seq (C-COMMIT-003); ayni_step derives tasks from goals in
 * transitions of their own. The log does not store these transitions yet,
 * so durable_seq never covers them and a reboot forgets them, and the next
 * boot gives their commit_seq to other transitions (ADR-0017,
 * C-COMMIT-016). Enumerations are uint32_t typedefs. Lifecycles, kinds,
 * channels and origins start at 1; a reason of 0 is AYNI_REASON_NONE, and an
 * object reference of all zeros names no object.
 */

/* A UTF-8 view with an explicit size and no terminator; data may be NULL
 * only when size is 0. Copied during the call that receives it. */
typedef struct ayni_string_v1 {
    const char *data;
    uint32_t size; /* bytes, not characters */
} ayni_string_v1;

/*
 * A catalog id, such as a goal type id, is 1 to AYNI_CATALOG_ID_MAX_BYTES
 * bytes of lowercase ASCII letters, digits, '.', '_' and '-'.
 */
#define AYNI_CATALOG_ID_MAX_BYTES 32u

/* Copied catalog ID; size 0 means no association, bytes beyond size are zero.
 * No NUL terminator is required. Since 0.32. */
typedef struct ayni_catalog_name_v1 {
    uint32_t size;                        /* Bytes in data, at most AYNI_CATALOG_ID_MAX_BYTES. */
    char data[AYNI_CATALOG_ID_MAX_BYTES]; /* Caller-owned immutable snapshot. */
} ayni_catalog_name_v1;

/* Object kinds an ayni_object_ref_v1 can name. */
typedef uint32_t ayni_object_kind_t;

#define AYNI_OBJECT_DEVICE ((ayni_object_kind_t)1)
#define AYNI_OBJECT_RESOURCE ((ayni_object_kind_t)2)
#define AYNI_OBJECT_CAPABILITY ((ayni_object_kind_t)3)
#define AYNI_OBJECT_BINDING ((ayni_object_kind_t)4)
#define AYNI_OBJECT_PROVIDER ((ayni_object_kind_t)5)
#define AYNI_OBJECT_GOAL_TYPE ((ayni_object_kind_t)6)
#define AYNI_OBJECT_STRATEGY ((ayni_object_kind_t)7)
#define AYNI_OBJECT_AGENT_INSTANCE ((ayni_object_kind_t)8)
#define AYNI_OBJECT_GOAL ((ayni_object_kind_t)9)
#define AYNI_OBJECT_TASK ((ayni_object_kind_t)10)
#define AYNI_OBJECT_ACTION ((ayni_object_kind_t)11)
#define AYNI_OBJECT_GRANT ((ayni_object_kind_t)12)
#define AYNI_OBJECT_SCENE ((ayni_object_kind_t)13)
#define AYNI_OBJECT_OBSERVATION ((ayni_object_kind_t)14)
#define AYNI_OBJECT_MEMORY ((ayni_object_kind_t)15)
#define AYNI_OBJECT_SUBSCRIPTION ((ayni_object_kind_t)16)
#define AYNI_OBJECT_PRINCIPAL ((ayni_object_kind_t)17)
#define AYNI_OBJECT_SESSION ((ayni_object_kind_t)18)
/* Managed logical call, including retained unknown outcomes. Since 0.31. */
#define AYNI_OBJECT_CALL ((ayni_object_kind_t)19)

/*
 * Reference to a versioned object (plan section 3.6). A reference carries no
 * permission: the receiver resolves it against the authenticated source and
 * checks its rights (C-WIRE-010).
 */
typedef struct ayni_object_ref_v1 {
    uint64_t origin_device_id; /* 0: this kernel */
    uint64_t id;
    uint32_t revision; /* 0: unknown, or the kind has no revision */
    ayni_object_kind_t kind;
} ayni_object_ref_v1;

/*
 * Why a call failed, beside its status (C-STATUS-006). Each reason belongs to
 * the status named in its comment; contracts/domain/status_codes.md lists
 * them. A published value is never reused for another meaning.
 */
typedef uint32_t ayni_reason_t;

#define AYNI_REASON_NONE ((ayni_reason_t)0) /* no detail beyond the status */
/* DENIED: session_id names no open session. */
#define AYNI_REASON_SESSION_UNKNOWN ((ayni_reason_t)1)
/* UNSUPPORTED: a nonzero command_id, before 0.14; no longer returned. */
#define AYNI_REASON_COMMAND_ID_UNSUPPORTED ((ayni_reason_t)2)
/* UNSUPPORTED: an intent kind this library does not implement. */
#define AYNI_REASON_INTENT_UNSUPPORTED ((ayni_reason_t)3)
/* NOT_FOUND: no goal type has this id. */
#define AYNI_REASON_GOAL_TYPE_UNKNOWN ((ayni_reason_t)4)
/* UNSUPPORTED_VERSION: the goal type id is known, its version is not. */
#define AYNI_REASON_GOAL_TYPE_VERSION ((ayni_reason_t)5)
/* INVALID_ARGUMENT: goal parameters too large, or refused by the goal type. */
#define AYNI_REASON_GOAL_PARAMETERS ((ayni_reason_t)6)
/* NOT_FOUND: owner_instance_id names no agent instance. */
#define AYNI_REASON_OWNER_UNKNOWN ((ayni_reason_t)7)
/* NOT_FOUND: parent_goal_id names no goal. */
#define AYNI_REASON_PARENT_UNKNOWN ((ayni_reason_t)8)
/* INVALID_STATE: the parent goal is neither ACTIVE nor PAUSED. */
#define AYNI_REASON_PARENT_STATE ((ayni_reason_t)9)
/* INVALID_ARGUMENT: a child goal must have its parent's owner. */
#define AYNI_REASON_PARENT_OWNER ((ayni_reason_t)10)
/* CAPACITY_EXCEEDED: the child would be deeper than goal_depth_max. */
#define AYNI_REASON_GOAL_DEPTH ((ayni_reason_t)11)
/* CAPACITY_EXCEEDED: the parent has goal_children_max live children. */
#define AYNI_REASON_GOAL_CHILDREN ((ayni_reason_t)12)
/* CAPACITY_EXCEEDED: goal_capacity goals exist; since 0.13, none of them
 * terminal (ADR-0025). */
#define AYNI_REASON_GOAL_CAPACITY ((ayni_reason_t)13)
/* CAPACITY_EXCEEDED: this boot has assigned every id it can (C-ID-010). */
#define AYNI_REASON_ID_EXHAUSTED ((ayni_reason_t)14)
/* NOT_FOUND: goal_id names no goal. */
#define AYNI_REASON_GOAL_UNKNOWN ((ayni_reason_t)15)
/* REVISION_CONFLICT: expected_revision is not the goal's revision. */
#define AYNI_REASON_GOAL_REVISION ((ayni_reason_t)16)
/* INVALID_STATE: the goal's lifecycle does not allow this change. */
#define AYNI_REASON_GOAL_STATE ((ayni_reason_t)17)
/* CAPACITY_EXCEEDED: a goal, task or instance the change would raise is at
 * the largest revision. */
#define AYNI_REASON_REVISION_EXHAUSTED ((ayni_reason_t)18)
/* Since 0.6 (ADR-0018). */
/* INVALID_ARGUMENT: the goal type does not accept the trigger kind. */
#define AYNI_REASON_TRIGGER_KIND ((ayni_reason_t)19)
/* INVALID_STATE: the owner instance is STOPPED or FAULTED. */
#define AYNI_REASON_OWNER_STATE ((ayni_reason_t)20)
/* NOT_FOUND: task_id names no task in memory. */
#define AYNI_REASON_TASK_UNKNOWN ((ayni_reason_t)21)
/* REVISION_CONFLICT: expected_revision is not the task's revision. */
#define AYNI_REASON_TASK_REVISION ((ayni_reason_t)22)
/* INVALID_STATE: the task's lifecycle does not allow this change. */
#define AYNI_REASON_TASK_STATE ((ayni_reason_t)23)
/* NOT_FOUND: instance_id names no agent instance. */
#define AYNI_REASON_INSTANCE_UNKNOWN ((ayni_reason_t)24)
/* REVISION_CONFLICT: expected_revision is not the instance's revision. */
#define AYNI_REASON_INSTANCE_REVISION ((ayni_reason_t)25)
/* INVALID_STATE: the instance's lifecycle does not allow this change. */
#define AYNI_REASON_INSTANCE_STATE ((ayni_reason_t)26)
/* Since 0.8. In ayni_goal_info_v1 only: the task table has no room for the
 * task an occurrence would become (ADR-0020). */
#define AYNI_REASON_TASK_CAPACITY ((ayni_reason_t)27)
/* Since 0.10. In ayni_task_info_v1 only, for a task its strategy's decision
 * holds (ADR-0022). INTENT_UNSUPPORTED, TASK_REVISION and
 * REVISION_EXHAUSTED hold tasks too. */
/* CAPACITY_EXCEEDED: the decision holds more intents than the strategy's
 * max_intents_per_call, or the strategy returned AYNI_CAPACITY_EXCEEDED;
 * since 0.12 also a private checkpoint larger than its
 * max_checkpoint_bytes. */
#define AYNI_REASON_POLICY_OUTPUT_LIMIT ((ayni_reason_t)28)
/* INVALID_ARGUMENT: an intent of the decision has another struct_size, or
 * two of its intents would each end the task; since 0.12 also a step_key
 * that is not a catalog id on a REPORT_PROGRESS intent, or any step_key on
 * an intent that ends the task. */
#define AYNI_REASON_POLICY_INTENT ((ayni_reason_t)29)
/* INVALID_ARGUMENT: the strategy returned AYNI_OK without an intent, which
 * leaves the task with nothing to wait for. */
#define AYNI_REASON_POLICY_NO_PROGRESS ((ayni_reason_t)30)
/* Since 0.10. INVALID_STATE, in ayni_goal_info_v1 only: its strategy's
 * decision holds a task of the goal, which keeps the goal from deriving
 * another until that task ends (ADR-0022). */
#define AYNI_REASON_TASK_HELD ((ayni_reason_t)31)
/* Since 0.12. In ayni_task_info_v1 only, for a task its strategy's decision
 * holds (ADR-0024). */
/* INVALID_ARGUMENT: a REPORT_PROGRESS intent names a step the task, or an
 * earlier intent of the decision, already commits (C-POLICY-008). */
#define AYNI_REASON_STEP_TAKEN ((ayni_reason_t)32)
/* CAPACITY_EXCEEDED: the decision would leave the task with more steps than
 * task_step_capacity. */
#define AYNI_REASON_STEP_CAPACITY ((ayni_reason_t)33)
/* Since 0.14 (ADR-0026). NOT_FOUND, in ayni_agent_info_v1 only: the log
 * holds the instance of an agent this boot did not register, so it is
 * FAULTED. */
#define AYNI_REASON_AGENT_UNREGISTERED ((ayni_reason_t)34)
/* UNSUPPORTED_VERSION: a restored goal with a trigger whose goal type names
 * no strategy now (ayni_goal_info_v1 hold_reason, and its tasks'), or a
 * restored task whose private checkpoint was written by another strategy
 * or version than the one its goal type names now (ayni_task_info_v1). */
#define AYNI_REASON_STRATEGY_CHANGED ((ayni_reason_t)35)
/* EXPIRED: the command_id is not mapped and at or below the principal's
 * floor, so the command may have been accepted once (ayni_submit_intent). */
#define AYNI_REASON_COMMAND_EXPIRED ((ayni_reason_t)36)
/* IDEMPOTENCY_CONFLICT: the principal's command with this command_id was
 * accepted with other content. */
#define AYNI_REASON_COMMAND_CONFLICT ((ayni_reason_t)37)
/* BUSY: the storage buffer has no room for another transition until the
 * host stores the batches before it. */
#define AYNI_REASON_STORAGE_BACKLOG ((ayni_reason_t)38)
/* Since 0.15. STORAGE_FAILED: not enough usable space for a new transition
 * and its maintenance reserve; previously accepted commands remain readable. */
#define AYNI_REASON_STORAGE_SPACE ((ayni_reason_t)39)
/* Since 0.16. DENIED: historical unconfirmed-work diagnostic. Reserved since
 * 0.27: schema 2 requires complete facts and does not install this boot hold. */
#define AYNI_REASON_LEGACY_UNCONFIRMED ((ayni_reason_t)40)

/* Since 0.18. Classified Grant failures; reasons retain one owning status. */
/* NOT_FOUND: no capability has this catalog ID. */
#define AYNI_REASON_CAPABILITY_UNKNOWN ((ayni_reason_t)41)
/* UNSUPPORTED_VERSION: the capability ID is known, its version is not. */
#define AYNI_REASON_CAPABILITY_VERSION ((ayni_reason_t)42)
/* NOT_FOUND: no retained Grant has this ID. */
#define AYNI_REASON_GRANT_UNKNOWN ((ayni_reason_t)43)
/* REVISION_CONFLICT: the parent Grant revision differs. */
#define AYNI_REASON_GRANT_REVISION ((ayni_reason_t)44)
/* INVALID_ARGUMENT: malformed scope, lifetime or root revision. */
#define AYNI_REASON_GRANT_INPUT ((ayni_reason_t)45)
/* DENIED: delegated scope exceeds its parent. */
#define AYNI_REASON_GRANT_SCOPE ((ayni_reason_t)46)
/* DENIED: the parent chain cannot delegate this authority. */
#define AYNI_REASON_GRANT_PARENT ((ayni_reason_t)47)
/* CAPACITY_EXCEEDED: delegation exceeds the configured depth. */
#define AYNI_REASON_GRANT_DEPTH ((ayni_reason_t)48)
/* CAPACITY_EXCEEDED: no free or safely reclaimable Grant slot. */
#define AYNI_REASON_GRANT_CAPACITY ((ayni_reason_t)49)
/* BUSY: an authorization fact has not reached durable acceptance. */
#define AYNI_REASON_GRANT_NOT_DURABLE ((ayni_reason_t)50)
/* DENIED: the Grant is expired or belongs to an older limited boot. */
#define AYNI_REASON_GRANT_EXPIRED ((ayni_reason_t)51)
/* UNSUPPORTED_VERSION: the exact capability domain has changed. */
#define AYNI_REASON_GRANT_CATALOG_CHANGED ((ayni_reason_t)52)
/* DENIED: the receiving instance is unavailable. */
#define AYNI_REASON_GRANT_SUBJECT ((ayni_reason_t)53)
/* DENIED: authorization requires a trusted management Session. */
#define AYNI_REASON_GRANT_CHANNEL ((ayni_reason_t)54)
/* UNSUPPORTED: Scene or quota authorization is not implemented. */
#define AYNI_REASON_GRANT_UNSUPPORTED ((ayni_reason_t)55)
/* The target or an ancestor is revoked or protected by its deny gate. Since 0.19. */
#define AYNI_REASON_GRANT_REVOKED ((ayni_reason_t)56)
/* Another accepted revocation already targets this Grant. Since 0.19. */
#define AYNI_REASON_REVOCATION_PENDING ((ayni_reason_t)57)
/* Accepted revocations pin every result slot for this principal. Since 0.19. */
#define AYNI_REASON_COMMAND_PINNED ((ayni_reason_t)58)
/* The bounded deny-gate or pending-control storage is full. Since 0.19. */
#define AYNI_REASON_REVOCATION_CAPACITY ((ayni_reason_t)59)

/* Classified failure detail. Carries a reason and the object it concerns,
 * never credentials, private memory text, media or personal data. */
typedef struct ayni_error_v1 {
    uint32_t struct_size;
    ayni_status_t status; /* the call's return value */
    ayni_reason_t reason;
    uint32_t reserved;         /* written as 0 */
    ayni_object_ref_v1 object; /* all 0 when no object is concerned */
} ayni_error_v1;

/* Since 0.21. INVALID_ARGUMENT: malformed requirement or exporter failure. */
#define AYNI_REASON_GOAL_REQUIREMENT_INPUT ((ayni_reason_t)60)
/* Since 0.21. CAPACITY_EXCEEDED: declared requirements exceed configured storage. */
#define AYNI_REASON_GOAL_REQUIREMENT_CAPACITY ((ayni_reason_t)61)
/* Since 0.21. DENIED: no selected Grant covers this exact requirement. */
#define AYNI_REASON_GOAL_AUTHORITY_MISSING ((ayni_reason_t)62)
/* Since 0.21. DENIED: an explicit authorization resume is required. */
#define AYNI_REASON_GOAL_AUTHORITY_PAUSED ((ayni_reason_t)63)
/* Since 0.21. DENIED: child requirements exceed their parent's saved ceiling. */
#define AYNI_REASON_GOAL_AUTHORITY_PARENT ((ayni_reason_t)64)
/* Since 0.22: Grant is bound to another, ended or missing Goal. */
#define AYNI_REASON_GRANT_GOAL ((ayni_reason_t)65)
/* Since 0.23: the saved Goal identity or original missing-item set changed. */
#define AYNI_REASON_GOAL_APPROVAL_CHANGED ((ayni_reason_t)66)
/* Since 0.26. DENIED: reauthorization's immutable creation echo changed. */
#define AYNI_REASON_GOAL_REAUTHORIZATION_CHANGED ((ayni_reason_t)67)
/* Since 0.26. REVISION_CONFLICT: authorization facts changed after the caller read them. */
#define AYNI_REASON_GOAL_AUTHORITY_REVISION ((ayni_reason_t)68)
/* Since 0.26. BUSY: the replacement authorization binding is not durable yet. */
#define AYNI_REASON_GOAL_AUTHORITY_NOT_DURABLE ((ayni_reason_t)69)
/* Since 0.30. CAPACITY_EXCEEDED: Task model calls exhausted; owner may raise its limit. */
#define AYNI_REASON_TASK_MODEL_CALL_LIMIT ((ayni_reason_t)70)

/* Since 0.31. RESULT_UNKNOWN: a terminal Provider receipt violated its declared result contract. */
#define AYNI_REASON_PROVIDER_RESULT ((ayni_reason_t)71)
/* Since 0.31. EXPIRED: ordinary resource waiting reached its configured deadline. */
#define AYNI_REASON_RESOURCE_WAIT ((ayni_reason_t)72)
/* Since 0.33. INVALID_ARGUMENT: requested period is faster than the trusted background floor. */
#define AYNI_REASON_BACKGROUND_FREQUENCY ((ayni_reason_t)73)
/* Required Scene is disabled, changed or not yet durable. Since 0.35. */
#define AYNI_REASON_EXECUTION_SCENE ((ayni_reason_t)74)
/* Exact measured prerequisite is absent, changed or expired. Since 0.35. */
#define AYNI_REASON_EXECUTION_OBSERVATION ((ayni_reason_t)75)

/* Principals: who a session acts for (C-AUTH-010). */
typedef uint32_t ayni_principal_kind_t;

#define AYNI_PRINCIPAL_OWNER ((ayni_principal_kind_t)1) /* the one owner (C-PROD-001) */

typedef struct ayni_principal_desc_v1 {
    uint32_t struct_size;
    ayni_principal_kind_t kind;
    uint64_t principal_id;     /* nonzero */
    uint32_t binding_revision; /* nonzero; of the owner binding that vouches for it */
    uint32_t reserved;         /* must be 0 */
} ayni_principal_desc_v1;

/* The input channel a host authenticated a session on. */
typedef uint32_t ayni_channel_t;

#define AYNI_CHANNEL_VOICE ((ayni_channel_t)1)      /* push-to-talk on the device */
#define AYNI_CHANNEL_MANAGEMENT ((ayni_channel_t)2) /* the bound management client */

typedef struct ayni_session_desc_v1 {
    uint32_t struct_size;
    ayni_channel_t channel;
    uint64_t principal_id; /* a registered principal */
    /* Since 0.16. The binding the Host authenticated; both fields required. */
    uint32_t binding_revision;
    uint32_t reserved; /* 0 */
} ayni_session_desc_v1;

/* An agent: a logical identity with one resident instance on this kernel. */
typedef struct ayni_agent_desc_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* must be 0 */
    uint64_t agent_id; /* nonzero */
} ayni_agent_desc_v1;

typedef uint32_t ayni_agent_lifecycle_t;

#define AYNI_AGENT_REGISTERED ((ayni_agent_lifecycle_t)1)
#define AYNI_AGENT_STARTING ((ayni_agent_lifecycle_t)2)
#define AYNI_AGENT_RUNNING ((ayni_agent_lifecycle_t)3)
#define AYNI_AGENT_PAUSED ((ayni_agent_lifecycle_t)4)
#define AYNI_AGENT_STOPPING ((ayni_agent_lifecycle_t)5)
#define AYNI_AGENT_STOPPED ((ayni_agent_lifecycle_t)6)
#define AYNI_AGENT_FAULTED ((ayni_agent_lifecycle_t)7)

typedef struct ayni_agent_info_v1 {
    uint32_t struct_size;
    ayni_agent_lifecycle_t lifecycle;
    uint64_t agent_id;
    /* 0 until the kernel first enters RUNNING; since 0.14 the log keeps it
     * across restarts (ADR-0026). */
    uint64_t instance_id;
    uint64_t commit_seq; /* of the last change to the instance; 0 before RUNNING */
    uint32_t revision;   /* since 0.6; 1 when the instance starts; 0 before RUNNING */
    /* Since 0.14. AYNI_REASON_AGENT_UNREGISTERED for an instance FAULTED
     * because its agent is no longer registered; NONE otherwise. */
    ayni_reason_t fault_reason;
} ayni_agent_info_v1;

/*
 * Checks the parameters of a goal before it is accepted (C-POLICY-009).
 * Returns AYNI_OK to accept them; any other status refuses the goal. Called
 * on the kernel owner thread inside ayni_submit_intent; parameters are valid
 * only during the call. It must not call kernel functions, block or perform
 * I/O (C-POLICY-001). If it calls one that changes the kernel, that call
 * returns AYNI_INVALID_STATE without effect, so the goal it checks cannot
 * change before it is accepted.
 */
typedef ayni_status_t (*ayni_goal_validate_fn)(void *ctx, ayni_bytes_v1 parameters);

/*
 * Since 0.6. When a goal produces tasks (ADR-0018). A goal's tasks advance
 * only while the goal and all its ancestors are ACTIVE and its owner
 * instance is RUNNING; ayni_step then derives each due occurrence as a task.
 *
 * Since 0.7 (ADR-0019). A goal's occurrences are numbered from 1 in the
 * order they fall due. Times are on the host's monotonic clock and keep
 * running while the goal cannot advance; they are not calendar times
 * (C-TIME-007). Occurrences that fall due while the goal cannot advance, or
 * before a step runs, wait and become one task, which names the latest of
 * them and how many it stands for (C-PROD-018, C-LIMIT-007); an interval
 * goal then keeps its schedule.
 *
 * Since 0.8 (ADR-0020). While the task table has no room or no id is left,
 * or, since 0.10, while its strategy's decision holds a task of the goal
 * (ADR-0022), a due occurrence of a goal that may advance waits for at most
 * its goal type's occurrence_wait_max_us, counted from when it fell due;
 * then the step that finds its wait over skips every occurrence of the goal
 * due by the step's start, counting them and giving the reason (C-RES-005).
 * Since 0.11, when the goal then has no occurrence to come and no task that
 * has not ended, as after a ONCE or DELAY goal's one occurrence, the same
 * transition makes it FAILED, keeping the skip's reason in skip_reason,
 * and cancels its descendants that are not terminal with their tasks
 * (ADR-0023); a goal whose end would raise a revision past the largest
 * stays ACTIVE instead.
 * A goal created with an expiry becomes EXPIRED at that time, paused or not,
 * together with its descendants and their tasks that have not ended; a goal
 * expiring takes no occurrence due at the same step.
 *
 * Since 0.9 (ADR-0021). A LOCAL_INPUT goal's occurrences are the local
 * inputs from its trigger_source that ayni_step takes while the goal is not
 * terminal, paused or not (ayni_post_event); each falls due when the step
 * that takes it started. They wait and become one task like any other.
 */
typedef uint32_t ayni_trigger_kind_t;

#define AYNI_TRIGGER_NONE ((ayni_trigger_kind_t)0) /* no tasks of its own */
#define AYNI_TRIGGER_ONCE ((ayni_trigger_kind_t)1) /* one occurrence, due at creation */
/* Since 0.7. One occurrence, due trigger_delay_us after creation. */
#define AYNI_TRIGGER_DELAY ((ayni_trigger_kind_t)2)
/* Since 0.7. The first occurrence is due trigger_delay_us after creation,
 * each next one trigger_period_us after the one before. */
#define AYNI_TRIGGER_INTERVAL ((ayni_trigger_kind_t)3)
/* Since 0.9. One occurrence for each local input from trigger_source. */
#define AYNI_TRIGGER_LOCAL_INPUT ((ayni_trigger_kind_t)4)
/* Since 0.7. The shortest trigger_period_us: goals work on a scale of
 * seconds (plan §2.1), and occurrence numbers then last beyond any device's
 * life. */
#define AYNI_TRIGGER_PERIOD_MIN_US 1000000u
/* The bit of a trigger kind in ayni_goal_type_v1.trigger_kinds. */
#define AYNI_TRIGGER_BIT(kind) (1u << (kind))
/* Since 0.8. An occurrence_wait_max_us under which occurrences are never
 * skipped: they wait until a task can be derived. */
#define AYNI_OCCURRENCE_WAIT_UNBOUNDED UINT64_MAX

/* Since 0.21. Emit each requirement through the borrowed sink in authority.h.
 * Called on the owner thread after parameter validation, without reentry.
 * Emitted inputs are copied before emit returns; neither sink nor ctx may be
 * retained. Any callback or emit failure refuses the whole Goal atomically.
 * An export with no emits explicitly declares zero requirements. */
struct ayni_goal_requirement_sink_v1;
typedef ayni_status_t (*ayni_goal_requirements_fn)(
    void *ctx, ayni_bytes_v1 parameters, const struct ayni_goal_requirement_sink_v1 *sink);

typedef uint32_t ayni_work_class_t;
/* Normal priority; resource timeout pauses the Task. Since 0.33. */
#define AYNI_WORK_ORDINARY ((ayni_work_class_t)0)
/* User interaction priority; resource timeout pauses the Task. Since 0.33. */
#define AYNI_WORK_FOREGROUND ((ayni_work_class_t)1)
/* Due reminder priority; resource wait is retained without timeout. Since 0.33. */
#define AYNI_WORK_REMINDER ((ayni_work_class_t)2)
/* Lowest priority, periodic only; resource timeout skips this Task. Since 0.33. */
#define AYNI_WORK_BACKGROUND ((ayni_work_class_t)3)
/* Default background period floor, in microseconds (5 minutes). Since 0.33. */
#define AYNI_BACKGROUND_PERIOD_DEFAULT_US UINT64_C(300000000)
/* Ordinary/foreground/background resource wait, in microseconds. Since 0.33. */
#define AYNI_RESOURCE_WAIT_DEFAULT_US UINT64_C(30000000)

/* Trusted GoalType scheduling profile; all zero preserves ordinary behavior. Since 0.33. */
typedef struct ayni_work_profile_v1 {
    ayni_work_class_t work_class;      /* One of WORK_*; never selected by a policy intent. */
    uint32_t reserved;                 /* Must be zero. */
    uint64_t background_period_min_us; /* BACKGROUND: 0 selects default, otherwise finite >= trigger
                                          minimum; other classes: 0. */
} ayni_work_profile_v1;

typedef struct ayni_goal_type_v1 {
    uint32_t struct_size;
    uint32_t version;               /* nonzero */
    ayni_string_v1 goal_type_id;    /* a catalog id, e.g. "reminder" */
    void *ctx;                      /* caller-owned until ayni_destroy returns AYNI_OK */
    ayni_goal_validate_fn validate; /* NULL: the goal type takes no parameters */
    /* Since 0.6. The trigger kinds its goals may choose: AYNI_TRIGGER_BIT of
     * each, at least one, only kinds this library defines. */
    uint32_t trigger_kinds;
    uint32_t reserved; /* must be 0 */
    /* Since 0.8. How long a due occurrence may wait for a task before it is
     * skipped (ADR-0020): 0 skips it at once, AYNI_OCCURRENCE_WAIT_UNBOUNDED
     * never. */
    uint64_t occurrence_wait_max_us;
    /* Since 0.10. The id of the strategy that decides its goals' tasks
     * (ayni_register_strategy), resolved by ayni_start. Empty exactly when
     * trigger_kinds holds NONE alone, since such goals have no tasks. */
    ayni_string_v1 strategy_id;
    /* Since 0.21. NULL explicitly declares zero needs for newly created Goals;
     * it does not reinterpret absent authorization metadata in older records. */
    ayni_goal_requirements_fn export_requirements;
    /* Since 0.33. Copied into each Goal; changing it requires a new type version.
     * REMINDER requires unbounded occurrence_wait_max_us; BACKGROUND allows INTERVAL only
     * and requires occurrence_wait_max_us <= RESOURCE_WAIT_DEFAULT_US. */
    ayni_work_profile_v1 work_profile;
} ayni_goal_type_v1;

typedef uint32_t ayni_goal_lifecycle_t;

#define AYNI_GOAL_PROPOSED ((ayni_goal_lifecycle_t)1)
#define AYNI_GOAL_ACTIVE ((ayni_goal_lifecycle_t)2)
#define AYNI_GOAL_PAUSED ((ayni_goal_lifecycle_t)3)
#define AYNI_GOAL_SATISFIED ((ayni_goal_lifecycle_t)4)
#define AYNI_GOAL_CANCELLED ((ayni_goal_lifecycle_t)5)
#define AYNI_GOAL_EXPIRED ((ayni_goal_lifecycle_t)6)
#define AYNI_GOAL_FAILED ((ayni_goal_lifecycle_t)7)

/* Where a goal came from (contracts/domain/primitives.md). */
typedef uint32_t ayni_origin_t;

#define AYNI_ORIGIN_USER ((ayni_origin_t)1)
#define AYNI_ORIGIN_RULE ((ayni_origin_t)2)
#define AYNI_ORIGIN_REFLECTION ((ayni_origin_t)3)
#define AYNI_ORIGIN_REMOTE_PRINCIPAL ((ayni_origin_t)4)

typedef struct ayni_goal_info_v1 {
    uint32_t struct_size;
    ayni_goal_lifecycle_t lifecycle;
    uint64_t goal_id;
    uint64_t parent_goal_id; /* 0: a top-level goal */
    uint64_t owner_instance_id;
    uint64_t origin_principal_id;
    uint64_t transition_id; /* that created the goal, in whichever boot did */
    uint64_t commit_seq;    /* of the last change; durable once durable_seq covers it */
    uint32_t revision;
    uint32_t cancel_revision; /* the revision that cancelled it; 0 if not cancelled */
    ayni_origin_t origin;
    ayni_channel_t origin_channel;
    uint32_t origin_binding_revision;
    uint32_t depth; /* 0 for a top-level goal */
    uint32_t goal_type_version;
    uint32_t goal_type_id_size;
    char goal_type_id[AYNI_CATALOG_ID_MAX_BYTES]; /* not terminated */
    ayni_trigger_kind_t trigger;                  /* since 0.6 */
    /* Since 0.6; since 0.7 the number of the latest occurrence derived, and
     * since 0.8 derived or skipped; 0 before the first. */
    uint32_t occurrences;
    uint64_t last_task_id;      /* since 0.6; 0 before the first task */
    uint64_t trigger_delay_us;  /* since 0.7; as created */
    uint64_t trigger_period_us; /* since 0.7; as created */
    /* Since 0.7. When the next occurrence falls due, on the host monotonic
     * clock; UINT64_MAX when none will: the trigger has none left or the
     * goal is terminal. Since 0.9, LOCAL_INPUT: when the earliest recorded
     * occurrence not yet taken fell due; UINT64_MAX while none waits. */
    uint64_t next_due_us;
    /* Since 0.8. When the goal expires, on the host monotonic clock;
     * UINT64_MAX when it never will: it has no expiry, its expiry was
     * dropped (ayni_step), or it is terminal. */
    uint64_t expires_us;
    uint32_t occurrences_skipped; /* since 0.8; occurrences that waited too long */
    /* Since 0.8. Why the latest skipped occurrences could not become a task:
     * AYNI_REASON_TASK_CAPACITY, AYNI_REASON_ID_EXHAUSTED or, since 0.10,
     * AYNI_REASON_TASK_HELD; NONE before any. Since 0.11 a goal that
     * becomes FAILED when its last occurrence is skipped keeps that skip's
     * reason here (ayni_trigger_kind_t). */
    ayni_reason_t skip_reason;
    /* Since 0.8. While the goal has an occurrence to come, what would keep it
     * from becoming a task now, as skip_reason; AYNI_REASON_NONE when
     * nothing would or none will come. A pause keeping it back shows in
     * the lifecycles, not here. A LOCAL_INPUT goal has one to come while an
     * occurrence waits. */
    ayni_reason_t wait_reason;
    /* Since 0.14 (ADR-0026). Why this boot holds a goal the log restored
     * that is not terminal: AYNI_REASON_GOAL_TYPE_UNKNOWN when no goal type
     * has its goal_type_id, AYNI_REASON_GOAL_TYPE_VERSION when none has it
     * at goal_type_version, AYNI_REASON_STRATEGY_CHANGED when its type names
     * no strategy now and the goal has a trigger. A held goal neither
     * expires nor takes occurrences or local input, and its tasks are held,
     * until a boot can serve it again; next_due_us and expires_us show what
     * the log has pending. It can still be cancelled, which ends the hold.
     * NONE otherwise. */
    ayni_reason_t hold_reason;
    uint32_t trigger_source_size; /* since 0.9; 0 unless the trigger is LOCAL_INPUT */
    char trigger_source[AYNI_CATALOG_ID_MAX_BYTES]; /* since 0.9; as created, not terminated */
    /* Since 0.23: original relative expiry in microseconds; 0 means never.
     * This immutable value is independent of the current boot's expires_us. */
    uint64_t original_expiry_us;
    /* Since 0.33. Immutable trusted scheduling semantics from creation. */
    ayni_work_profile_v1 work_profile;
} ayni_goal_info_v1;

/*
 * Since 0.6. Task lifecycle (contracts/domain/lifecycles.md). This library
 * reaches READY, PAUSED, CANCELLED, since 0.8 EXPIRED and, since 0.10,
 * COMPLETED and FAILED; RUNNING and CANCELLING need requests a task waits
 * for (ADR-0018, ADR-0020, ADR-0022).
 */
typedef uint32_t ayni_task_lifecycle_t;

#define AYNI_TASK_READY ((ayni_task_lifecycle_t)1)
#define AYNI_TASK_RUNNING ((ayni_task_lifecycle_t)2)
#define AYNI_TASK_PAUSED ((ayni_task_lifecycle_t)3)
#define AYNI_TASK_CANCELLING ((ayni_task_lifecycle_t)4)
#define AYNI_TASK_COMPLETED ((ayni_task_lifecycle_t)5)
#define AYNI_TASK_CANCELLED ((ayni_task_lifecycle_t)6)
#define AYNI_TASK_FAILED ((ayni_task_lifecycle_t)7)
#define AYNI_TASK_EXPIRED ((ayni_task_lifecycle_t)8)

/* Since 0.6. A task of a goal; its owner is the goal's (C-LIFE-020). */
typedef struct ayni_task_info_v1 {
    uint32_t struct_size;
    ayni_task_lifecycle_t lifecycle;
    uint64_t task_id;
    uint64_t goal_id;
    uint64_t owner_instance_id;
    uint64_t transition_id; /* that created the task, in whichever boot did */
    uint64_t commit_seq;    /* of the last change; durable once durable_seq covers it */
    uint32_t revision;
    uint32_t cancel_revision; /* the revision that cancelled it; 0 if not cancelled */
    /* Since 0.6; since 0.7 the latest of the goal's occurrences it stands
     * for. */
    uint32_t occurrence;
    uint32_t occurrence_count; /* since 0.7; how many it stands for, at least 1 */
    /* Since 0.7. When the latest occurrence it stands for fell due, on the
     * host monotonic clock; since 0.14, for a task the log restored, when
     * recovery ended (ADR-0026). */
    uint64_t due_us;
    /* Since 0.10 (ADR-0022). Why its strategy's decision holds the task
     * PAUSED: the status and reason of the hold, the status being the
     * failure the strategy returned or the one the kernel refused its
     * decision with, never AYNI_OK. A held task stays PAUSED while its goal
     * and owner may advance, until it ends. AYNI_OK and AYNI_REASON_NONE for
     * a task never held. Since 0.14 a boot also holds a restored task it
     * cannot decide (ADR-0026), without changing its revision: its goal is
     * held, with AYNI_NOT_FOUND and AYNI_REASON_GOAL_TYPE_UNKNOWN, or
     * AYNI_UNSUPPORTED_VERSION and the goal's hold_reason; or its goal type
     * names another strategy or version than the one that wrote its private
     * checkpoint, with AYNI_UNSUPPORTED_VERSION and
     * AYNI_REASON_STRATEGY_CHANGED; or its
     * checkpoint is larger than that strategy's max_checkpoint_bytes, with
     * AYNI_CAPACITY_EXCEEDED and AYNI_REASON_POLICY_OUTPUT_LIMIT. A later
     * boot that has what was lacking finds the task as the log left it. */
    ayni_status_t hold_status;
    ayni_reason_t hold_reason;
    /* Since 0.12 (ADR-0024). The steps its strategy's decisions committed;
     * the strategy sees them in ayni_task_snapshot_v1. */
    uint32_t step_count;
    uint32_t reserved; /* written as 0 */
    /* Since 0.30. Accepted model request count; never reset by resume/raise. */
    uint32_t model_calls_used;
    /* Since 0.30. Durable positive soft ceiling; owner can only increase it. */
    uint32_t model_call_limit;
    /* Since 0.30. Positive startup ceiling; cannot be changed by an intent. */
    uint32_t model_call_hard_limit;
    /* Since 0.30. 1 if a new model request hit the ceiling, else 0. Raising
     * clears only this hold; other holds and progress remain unchanged. */
    uint32_t model_call_limit_reached;
    /* Since 0.31. Selected logical call; zero when no call blocks the next
     * strategy decision. It remains set while cancellation or outcome is unresolved. */
    uint64_t logical_call_id;
} ayni_task_info_v1;

/*
 * Intents a principal submits through a session (contracts/domain/
 * primitives.md). The kinds this library implements; the other planned
 * kinds are in include/ayni/draft/kernel_api.h.
 */
typedef uint32_t ayni_intent_kind_t;

#define AYNI_INTENT_GOAL_CREATE ((ayni_intent_kind_t)1) /* body: ayni_goal_create_v1 */
#define AYNI_INTENT_GOAL_CANCEL ((ayni_intent_kind_t)2) /* body: ayni_goal_target_v1 */
#define AYNI_INTENT_GOAL_PAUSE ((ayni_intent_kind_t)3)  /* body: ayni_goal_target_v1 */
#define AYNI_INTENT_GOAL_RESUME ((ayni_intent_kind_t)4) /* body: ayni_goal_target_v1 */
/* Since 0.6 (ADR-0018). */
#define AYNI_INTENT_TASK_CANCEL ((ayni_intent_kind_t)5)     /* body: ayni_task_target_v1 */
#define AYNI_INTENT_INSTANCE_PAUSE ((ayni_intent_kind_t)6)  /* body: ayni_instance_target_v1 */
#define AYNI_INTENT_INSTANCE_RESUME ((ayni_intent_kind_t)7) /* body: ayni_instance_target_v1 */
#define AYNI_INTENT_INSTANCE_STOP ((ayni_intent_kind_t)8)   /* body: ayni_instance_target_v1 */
#define AYNI_INTENT_GRANT_CREATE                                                                   \
    ((ayni_intent_kind_t)9) /* body: ayni_grant_create_v1, authority.h */
#define AYNI_INTENT_GRANT_REVOKE ((ayni_intent_kind_t)10) /* body: ayni_grant_revoke_v1 */
/* Since 0.23: atomically approve every originally missing Goal requirement. */
#define AYNI_INTENT_GOAL_APPROVE ((ayni_intent_kind_t)11) /* body: ayni_goal_approve_v1 */
/* Since 0.26: explicitly bind existing durable Grants for a Goal and its live Tasks. */
#define AYNI_INTENT_GOAL_REAUTHORIZE ((ayni_intent_kind_t)12) /* body: ayni_goal_reauthorize_v1 */

/* Since 0.30. Owner management command, exact idempotency and revision CAS.
 * Raises the soft model call ceiling of a live Task, preserving all progress.
 * Requires a nonzero command_id and bound MANAGEMENT session. */
#define AYNI_INTENT_TASK_RAISE_MODEL_LIMIT ((ayni_intent_kind_t)13)
/* Owner asks the original Provider to reconcile an unknown operation. Since 0.31. */
#define AYNI_INTENT_CALL_QUERY_RESULT ((ayni_intent_kind_t)14)
/* Resume only an expired resource wait, preserving call identity and other holds.
 * Since 0.31. Body: ayni_task_target_v1; owner management, nonzero command_id, Task revision. */
#define AYNI_INTENT_TASK_RESUME_WAIT ((ayni_intent_kind_t)15)
/* Owner enables/disables an exact Scene definition with persistent CAS. Since 0.35. */
#define AYNI_INTENT_SCENE_SET_ENABLED ((ayni_intent_kind_t)16)

/* Since 0.30. Body of TASK_RAISE_MODEL_LIMIT; copied during submission. */
typedef struct ayni_task_raise_model_limit_v1 {
    uint32_t struct_size;      /* sizeof this struct */
    uint32_t model_call_limit; /* strictly above current soft limit, <= startup hard limit */
    uint64_t task_id;          /* nonzero retained live Task */
} ayni_task_raise_model_limit_v1;

/*
 * One envelope for every intent kind. The principal and channel are the
 * session's, set when the host opened it (C-AUTH-010); an intent cannot
 * claim others.
 */
typedef struct ayni_intent_v1 {
    uint32_t struct_size;
    ayni_intent_kind_t kind;
    /* Management command idempotency key (C-WIRE-003); 0: none. Since 0.14
     * a nonzero id names a command of the session's principal, kept as
     * ayni_submit_intent says (ADR-0026). */
    uint64_t command_id;
    uint64_t session_id;
    /* GOAL_CREATE: 0. Other kinds: the current revision of the goal, task
     * or instance they name (C-COMMIT-012). */
    uint32_t expected_revision;
    uint32_t reserved; /* must be 0 */
    const void *body;  /* the kind's body struct, starting with its struct_size */
} ayni_intent_v1;

typedef struct ayni_goal_create_v1 {
    uint32_t struct_size;
    uint32_t goal_type_version;
    ayni_string_v1 goal_type_id;
    uint64_t owner_instance_id; /* fixed for the goal's life (C-LIFE-020) */
    uint64_t parent_goal_id;    /* 0: a top-level goal */
    ayni_bytes_v1 parameters;   /* at most goal_parameter_max_bytes; copied */
    /* Since 0.6. A kind this library defines and the goal type accepts. */
    ayni_trigger_kind_t trigger;
    uint32_t reserved; /* must be 0 */
    /* Since 0.7. DELAY and INTERVAL: from creation to the first occurrence;
     * a time past the clock's range never comes. 0 for other kinds. */
    uint64_t trigger_delay_us;
    /* Since 0.7. INTERVAL: at least AYNI_TRIGGER_PERIOD_MIN_US. 0 for other
     * kinds. */
    uint64_t trigger_period_us;
    /* Since 0.8. From creation to when the goal expires; 0: never. A time
     * past the clock's range never comes. */
    uint64_t expiry_us;
    /* Since 0.9. LOCAL_INPUT: a catalog id naming the local input whose
     * events are its occurrences (ayni_post_event). Empty for other kinds. */
    ayni_string_v1 trigger_source;
} ayni_goal_create_v1;

typedef struct ayni_goal_target_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* must be 0 */
    uint64_t goal_id;
} ayni_goal_target_v1;

/* Since 0.6. */
typedef struct ayni_task_target_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* must be 0 */
    uint64_t task_id;
} ayni_task_target_v1;

/* Since 0.6. */
typedef struct ayni_instance_target_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* must be 0 */
    uint64_t instance_id;
} ayni_instance_target_v1;

/* Since 0.19. PENDING has no transition, APPLIED is queued in this boot,
 * DURABLE is confirmed persistent. None acknowledges physical stopping. */
typedef uint32_t ayni_intent_phase_t;
#define AYNI_INTENT_PENDING ((ayni_intent_phase_t)1)
#define AYNI_INTENT_APPLIED ((ayni_intent_phase_t)2)
#define AYNI_INTENT_DURABLE ((ayni_intent_phase_t)3)

typedef uint32_t ayni_intent_effect_t;
#define AYNI_INTENT_EFFECT_NONE ((ayni_intent_effect_t)0) /* PENDING: no applied effect yet */
#define AYNI_INTENT_EFFECT_CHANGED ((ayni_intent_effect_t)1)
#define AYNI_INTENT_EFFECT_NO_CHANGE ((ayni_intent_effect_t)2)
#define AYNI_INTENT_EFFECT_TARGET_GONE ((ayni_intent_effect_t)3)

typedef struct ayni_intent_result_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* written as 0 */
    /* The Goal, Task, instance or Grant named or created by the intent,
     * with its revision after the intent. Other objects it changed, such as
     * the goal a TASK_CANCEL ends (since 0.11), are read with their query
     * functions. */
    ayni_object_ref_v1 object;
    /* Of the transition the intent applied; for a retried command, of the
     * transition that first applied it, in whichever boot did. */
    uint64_t commit_seq;
    /* Since 0.19. PENDING: commit_seq=0 and applied_boot_epoch=0.
     * DURABLE: original stable commit_seq and applied_boot_epoch=0. */
    ayni_intent_phase_t phase;
    ayni_intent_effect_t effect;
    uint64_t applied_boot_epoch;
} ayni_intent_result_v1;

/* Since 0.19. Fixed caller-owned diagnostic output. */
typedef struct ayni_intent_receipt_v1 {
    uint32_t struct_size;
    ayni_intent_phase_t phase;
    ayni_intent_effect_t effect;
    uint32_t reserved; /* written as 0 */
    uint64_t principal_id;
    uint64_t command_id;
    ayni_object_ref_v1 object;
    uint64_t commit_seq;
    uint64_t applied_boot_epoch;
} ayni_intent_receipt_v1;

/*
 * Registers an agent. The kernel creates its instance when it first enters
 * RUNNING (ADR-0017). Returns AYNI_OK; AYNI_INVALID_ARGUMENT for a NULL
 * pointer, a zero agent_id, a nonzero reserved field or an agent_id already
 * registered; AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for
 * struct_size; AYNI_INVALID_STATE outside CONFIGURING;
 * AYNI_CAPACITY_EXCEEDED once agent_capacity agents are registered.
 */
ayni_status_t ayni_register_agent(ayni_kernel_t *kernel, const ayni_agent_desc_v1 *agent);

/*
 * Registers one version of a goal type; several versions of one id may
 * coexist. Returns AYNI_OK; AYNI_INVALID_ARGUMENT for a NULL pointer, a zero
 * version, a goal_type_id that is not a catalog id, trigger_kinds that is 0
 * or holds a kind this library does not define, a nonzero reserved field, a
 * strategy_id that is not a catalog id, or is not empty when trigger_kinds
 * holds NONE alone, or an id and version already registered;
 * AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for struct_size;
 * AYNI_INVALID_STATE outside CONFIGURING; AYNI_CAPACITY_EXCEEDED once
 * goal_type_capacity versions are registered.
 */
ayni_status_t ayni_register_goal_type(ayni_kernel_t *kernel, const ayni_goal_type_v1 *goal_type);

/* ---- Strategies (since 0.10) --------------------------------------------
 *
 * A strategy decides the tasks of the goal types that name it
 * (contracts/host/policy_interface.md, ADR-0022). It is a bounded decision
 * function: ayni_step calls decide on the owner thread with a snapshot of
 * one READY task, and the strategy answers with intents in an output the
 * kernel provides. Since 0.12 a decision may commit steps of the task and
 * leave it READY to be decided again, and it returns the private
 * checkpoint that the task's next snapshot holds (ADR-0024). decide must
 * not call kernel functions, block or perform I/O (C-POLICY-001). Given the
 * same snapshot, its private checkpoint included, it gives the same
 * decision, reading no clock or state the snapshot does not hold
 * (C-POLICY-006). The shape serves deterministic replay and replacing
 * strategies; it is not a security boundary.
 */

/* Since 0.12. A task's private checkpoint as a strategy last returned it,
 * with that strategy's id and version (C-POLICY-007, ADR-0024). It holds
 * only the strategy's own state; the task's steps are the kernel's. */
typedef struct ayni_private_checkpoint_v1 {
    ayni_string_v1 strategy_id; /* empty before the first decision applied */
    uint32_t strategy_version;  /* 0 before the first decision applied */
    uint32_t reserved;          /* written as 0 */
    ayni_bytes_v1 state;
} ayni_private_checkpoint_v1;

/* Since 0.12. A step a decision of the task committed (C-POLICY-008,
 * ADR-0024). */
typedef struct ayni_task_step_v1 {
    ayni_string_v1 step_key; /* a catalog id, unique in the task */
} ayni_task_step_v1;

/* Since 0.31. Defined in calls.h; borrowed only during a strategy callback. */
struct ayni_call_request_v1;
struct ayni_task_call_result_v1;

/* What a strategy sees of a task. The views point into kernel memory and
 * are valid only during the decide call (C-POLICY-004). */
typedef struct ayni_task_snapshot_v1 {
    uint32_t struct_size;
    uint32_t revision; /* the task's; each intent names it (C-COMMIT-012) */
    uint64_t task_id;
    uint64_t goal_id;
    uint64_t owner_instance_id;
    uint64_t due_us;           /* when the latest occurrence it stands for fell due */
    uint64_t now_us;           /* when the step deciding it started */
    uint32_t occurrence;       /* the latest of the goal's occurrences it stands for */
    uint32_t occurrence_count; /* how many it stands for, at least 1 */
    uint32_t goal_type_version;
    uint32_t reserved; /* written as 0 */
    ayni_string_v1 goal_type_id;
    ayni_bytes_v1 goal_parameters; /* as the goal type accepted them */
    /* Since 0.12 (ADR-0024). As the last decision of the task that the
     * kernel applied returned it; the output starts with a copy. */
    ayni_private_checkpoint_v1 checkpoint;
    /* Since 0.12. The steps the task's decisions committed, in the order
     * they committed them: steps[0] to steps[step_count - 1]. A task may
     * commit step_capacity steps in all, the kernel's task_step_capacity. */
    uint32_t step_count;
    uint32_t step_capacity;
    const ayni_task_step_v1 *steps;
    /* Since 0.31. Read only this Task's retained result by stable step key.
     * Returned text borrows Core memory until decide returns; null if disabled. */
    const void *call_context;
    ayni_status_t (*call_result)(const void *context, ayni_string_v1 step_key,
                                 struct ayni_task_call_result_v1 *out);
} ayni_task_snapshot_v1;

/* The intent kinds this library implements; the other planned kinds are in
 * include/ayni/draft/policy_v1.h. REPORT_COMPLETION and REPORT_FAILURE end
 * the task; since 0.12, REPORT_PROGRESS commits a step of it. */
typedef uint32_t ayni_policy_intent_kind_t;

/* Since 0.31. One selected call per decision; REQUEST_MODEL requires MODEL class. */
#define AYNI_POLICY_REQUEST_CALL ((ayni_policy_intent_kind_t)2)
#define AYNI_POLICY_REQUEST_MODEL ((ayni_policy_intent_kind_t)3)

#define AYNI_POLICY_REPORT_PROGRESS ((ayni_policy_intent_kind_t)4)   /* a step is done */
#define AYNI_POLICY_REPORT_COMPLETION ((ayni_policy_intent_kind_t)5) /* the task is done */
#define AYNI_POLICY_REPORT_FAILURE ((ayni_policy_intent_kind_t)6)    /* it cannot be done */

typedef struct ayni_policy_intent_v1 {
    uint32_t struct_size;
    ayni_policy_intent_kind_t kind;
    uint32_t expected_revision; /* the snapshot's revision (C-COMMIT-012) */
    /* Since 0.12 (ADR-0024). REPORT_PROGRESS: the step it commits, a
     * catalog id. 0 for the kinds that end the task. */
    uint32_t step_key_size;
    char step_key[AYNI_CATALOG_ID_MAX_BYTES]; /* not terminated */
} ayni_policy_intent_v1;

/* Where decide writes its decision. The kernel provides intents and sets
 * intent_capacity to the strategy's max_intents_per_call; decide writes
 * intents[0] to intents[intent_count - 1] and sets intent_count. Since 0.12
 * the kernel also provides checkpoint, checkpoint_capacity bytes (the
 * strategy's max_checkpoint_bytes), holding a copy of the task's private
 * checkpoint, and sets checkpoint_size to its size: decide may leave both
 * or write the private checkpoint the decision returns there. The kernel
 * reads back only intent_count, checkpoint_size and its own buffers. */
typedef struct ayni_policy_output_v1 {
    uint32_t struct_size;
    uint32_t intent_capacity;
    uint32_t intent_count;
    uint32_t reserved; /* written as 0 */
    ayni_policy_intent_v1 *intents;
    uint32_t checkpoint_capacity; /* since 0.12 */
    uint32_t checkpoint_size;     /* since 0.12 */
    uint8_t *checkpoint;          /* since 0.12 */
    /* Since 0.31. Stage one request by immediate bounded copy. On success the
     * callback writes intents[0] and sets intent_count=1; do not mix requests
     * with other intents. No call is accepted until the complete decision is
     * validated. Context/callback may be used only within this decide call. */
    void *call_context;
    ayni_status_t (*request_call)(void *context, const struct ayni_call_request_v1 *request);
} ayni_policy_output_v1;

/*
 * Decides one task: returns AYNI_OK with the decision in output,
 * AYNI_CAPACITY_EXCEEDED when the decision needs more than intent_capacity
 * intents or, since 0.12, checkpoint_capacity bytes (C-POLICY-002), or any
 * other failure status when it cannot decide.
 *
 * The kernel applies the decision as one transition. Since 0.12 the
 * decision's REPORT_PROGRESS intents commit their steps, and its private
 * checkpoint replaces the task's. One intent that ends the task makes it
 * COMPLETED or FAILED. If its goal then has no other task that has not
 * ended and no occurrence to come, as a ONCE or DELAY goal after its one
 * task, the goal becomes SATISFIED or FAILED likewise, and its descendants
 * that are not terminal are CANCELLED with their tasks. Since 0.12 a
 * decision without such an intent leaves the task READY at its next
 * revision, to be decided again in a later step after the tasks READY
 * before it. So each decision applied ends the task or commits a step, and
 * a task takes at most task_step_capacity + 1 (ADR-0024). Any other
 * decision holds the task: it becomes PAUSED with the status and reason in
 * ayni_task_info_v1, keeps its steps and private checkpoint, and stays
 * PAUSED until it is cancelled or expires (C-POLICY-002, C-POLICY-003).
 * Meanwhile its goal derives no task: the goal's occurrences wait, and are
 * skipped, with AYNI_REASON_TASK_HELD. It is held when decide fails, with
 * the status decide returned and AYNI_REASON_POLICY_OUTPUT_LIMIT for
 * AYNI_CAPACITY_EXCEEDED, AYNI_REASON_NONE otherwise; with INVALID_ARGUMENT
 * and AYNI_REASON_NONE when decide returns a value above AYNI_OK, which is
 * no status; with CAPACITY_EXCEEDED and POLICY_OUTPUT_LIMIT when
 * intent_count exceeds intent_capacity or checkpoint_size exceeds
 * checkpoint_capacity; with INVALID_ARGUMENT and POLICY_NO_PROGRESS for
 * AYNI_OK without an intent; then, for each intent in turn, with
 * INVALID_ARGUMENT and POLICY_INTENT for another struct_size, UNSUPPORTED
 * and INTENT_UNSUPPORTED for a kind this library does not implement, and
 * REVISION_CONFLICT and TASK_REVISION for another expected_revision; for
 * REPORT_PROGRESS, with INVALID_ARGUMENT and POLICY_INTENT for a step_key
 * that is not a catalog id, INVALID_ARGUMENT and STEP_TAKEN for a step the
 * task or an earlier intent already commits, and CAPACITY_EXCEEDED and
 * STEP_CAPACITY for a step beyond task_step_capacity; for an intent that
 * ends the task, with INVALID_ARGUMENT and POLICY_INTENT for a nonzero
 * step_key_size or when an earlier intent already ends the task; and with
 * CAPACITY_EXCEEDED and REVISION_EXHAUSTED when ending the task would raise
 * a revision past the largest (C-COMMIT-012).
 */
typedef ayni_status_t (*ayni_decide_fn)(void *ctx, const ayni_task_snapshot_v1 *snapshot,
                                        ayni_policy_output_v1 *output);

typedef struct ayni_strategy_v1 {
    uint32_t struct_size;
    uint32_t version;              /* nonzero */
    ayni_string_v1 strategy_id;    /* a catalog id */
    void *ctx;                     /* caller-owned until ayni_destroy returns AYNI_OK */
    ayni_decide_fn decide;         /* not NULL */
    uint32_t max_intents_per_call; /* 1 to policy_intent_capacity */
    /* sizeof(ayni_policy_intent_v1) as the strategy was built: the stride
     * of the intents it writes, which a later version may lengthen. */
    uint32_t intent_size;
    /* Since 0.12 (ADR-0024). The largest private checkpoint it returns, 0
     * to task_checkpoint_max_bytes; 0 when it keeps none. */
    uint32_t max_checkpoint_bytes;
    uint32_t reserved; /* must be 0 */
} ayni_strategy_v1;

/*
 * Registers a strategy. One version of an id is registered at a time: goal
 * types name strategies by id, and a strategy is replaced only across a
 * restart (C-POLICY-010). ayni_step has it decide each task of those goal
 * types that was READY when the step began, so each such task once per
 * step at most, and none at the largest revision. Returns AYNI_OK;
 * AYNI_INVALID_ARGUMENT for a NULL pointer or decide, a zero version, a
 * strategy_id that is not a catalog id, a max_intents_per_call of 0 or
 * above policy_intent_capacity, a max_checkpoint_bytes above
 * task_checkpoint_max_bytes, a nonzero reserved field, or a strategy_id
 * already registered;
 * AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for struct_size, and
 * likewise for an intent_size below or above this library's
 * sizeof(ayni_policy_intent_v1); AYNI_INVALID_STATE outside CONFIGURING;
 * AYNI_CAPACITY_EXCEEDED once strategy_capacity strategies are registered.
 */
ayni_status_t ayni_register_strategy(ayni_kernel_t *kernel, const ayni_strategy_v1 *strategy);

/*
 * Closes an open session; intents can no longer name it. Returns AYNI_OK;
 * AYNI_INVALID_ARGUMENT if kernel is NULL; AYNI_INVALID_STATE in CONFIGURING
 * and STOPPED, and when called from a callback; AYNI_NOT_FOUND
 * when no open session has this id.
 */
ayni_status_t ayni_close_session(ayni_kernel_t *kernel, uint64_t session_id);

/*
 * Validates an intent and applies its transition in memory (C-COMMIT-003).
 * AYNI_OK means accepted, not durable: the transition is durable once
 * durable_seq reaches the commit_seq in out_result (C-COMMIT-004), which,
 * since 0.14, happens once the host has stored its batch (ADR-0026);
 * ayni_step appends it, and the host's wake is called so that the owner
 * steps. out_result (nullable) is written on AYNI_OK only. out_error
 * (nullable) is written on every return once its own struct_size is
 * accepted, with status AYNI_OK and no reason on success.
 *
 * Checks run in this order, and the first failure is returned:
 * 1. arguments: AYNI_INVALID_ARGUMENT for a NULL kernel or intent, kind 0,
 *    a nonzero reserved field, or a struct_size below its first layout;
 *    AYNI_UNSUPPORTED_VERSION for a struct_size larger than this library
 *    knows. Applies to intent, out_result, out_error and, for the kinds
 *    implemented here, the body, which must not be NULL. GOAL_CREATE also
 *    needs expected_revision 0, a goal_type_id that is a catalog id,
 *    parameters whose data is not NULL unless their size is 0, a trigger
 *    kind this library defines, trigger times that fit it (an INTERVAL
 *    period of at least AYNI_TRIGGER_PERIOD_MIN_US, zero where the kind
 *    takes none), and a trigger_source that is a catalog id for
 *    LOCAL_INPUT and empty for other kinds.
 * 2. AYNI_UNSUPPORTED: an intent kind this library does not implement.
 * 3. AYNI_INVALID_STATE when called from a callback; then
 *    AYNI_NOT_READY outside RUNNING, without queueing (C-LIFE-004).
 * 4. AYNI_DENIED: session_id names no open session. Until grants exist, a
 *    session of the owner may submit every implemented intent (ADR-0017).
 *    Since 0.16, Host-authenticated sessions require the exact durable
 *    owner binding (ayni/host_authority.h); a session never removes an
 *    authorization deny. Since 0.26, GOAL_REAUTHORIZE
 *    requires the current durable owner's MANAGEMENT Session, including
 *    an exact retry; its complete request contract is in ayni/authority.h.
 *    Since 0.30, TASK_RAISE_MODEL_LIMIT has the same MANAGEMENT requirement
 *    even on retry, requires nonzero command_id/task_id/expected_revision,
 *    and compares complete canonical bytes. Unknown Task: NOT_FOUND;
 *    stale revision: REVISION_CONFLICT; terminal Task: INVALID_STATE;
 *    exhausted revision: CAPACITY_EXCEEDED; a limit that does not strictly
 *    increase or exceeds the startup ceiling: INVALID_ARGUMENT.
 *    Since 0.14, a nonzero command_id is then looked up among the commands
 *    of the session's principal (C-WIRE-003, C-WIRE-004, ADR-0026): one
 *    accepted before with the same kind, expected_revision and body, over
 *    any session and across restarts, returns AYNI_OK and its first result
 *    again, applying nothing and calling no wake; with other content,
 *    AYNI_IDEMPOTENCY_CONFLICT; an id not mapped and at or below the
 *    principal's floor, AYNI_EXPIRED. Each principal keeps its last
 *    command_capacity accepted commands: a new one beyond that drops the one
 *    with the smallest id and raises the floor to it. A refused command is
 *    not kept, so its retry is judged anew.
 *    Since 0.15, a new command requiring an unavailable principal row
 *    returns CAPACITY_EXCEEDED before changing state. Then STORAGE_FAILED
 *    with STORAGE_SPACE if usable medium space cannot cover the transition
 *    and maintenance reserve; otherwise BUSY when the buffer is full or
 *    retry/snapshot maintenance has paused transitions. The original result
 *    of a mapped command is returned before all of these storage checks.
 * 5. The intent's own rules, each with the reason named in its comment
 *    above. GOAL_CREATE: the goal type (NOT_FOUND, UNSUPPORTED_VERSION) and
 *    whether it accepts the trigger (INVALID_ARGUMENT), the owner instance
 *    (NOT_FOUND, then INVALID_STATE when STOPPED or FAULTED), the parent goal
 *    (NOT_FOUND, INVALID_STATE, INVALID_ARGUMENT, CAPACITY_EXCEEDED for depth
 *    and children), capacity and ids (CAPACITY_EXCEEDED), then the
 *    parameters (INVALID_ARGUMENT). Every other kind: the goal, task or
 *    instance it names (NOT_FOUND), then its revision (REVISION_CONFLICT),
 *    then its lifecycle (INVALID_STATE), then the revisions it would raise
 *    (CAPACITY_EXCEEDED).
 * AYNI_INTERNAL reports a defect in this library, not in the call: the goal
 * table refused a goal the checks admitted. The goal is not created, but the
 * id and the commit_seq reserved for it are used up; since 0.14 that
 * commit_seq is stored as an empty batch.
 *
 * GOAL_CREATE creates an ACTIVE goal at revision 1; since 0.13, when the
 * goal table is full, in the place of the terminal goal that ended first,
 * which leaves with its tasks (ADR-0025). GOAL_PAUSE takes an ACTIVE goal
 * to PAUSED and GOAL_RESUME takes it back; neither changes its children.
 * Since 0.27 recovered user lifecycle follows the same controls; GOAL_RESUME
 * does not clear independent authorization or compatibility holds.
 * GOAL_CANCEL takes a goal that is not yet terminal, and every descendant
 * that is not yet terminal, to CANCELLED in one transition (C-LIFE-022).
 * TASK_CANCEL takes a READY or PAUSED task to CANCELLED;
 * since 0.11, when that leaves its goal with no task that has not ended and
 * no occurrence to come, as for a ONCE or DELAY goal's one task, the goal
 * and its descendants are cancelled with it as by GOAL_CANCEL (ADR-0023).
 * INSTANCE_PAUSE takes a RUNNING instance to PAUSED and INSTANCE_RESUME
 * takes it back; INSTANCE_STOP takes a RUNNING or PAUSED instance to
 * STOPPED and cancels every goal and task it owns that is not yet terminal.
 *
 * In the same transition, every READY or PAUSED task of the named goal or
 * of one of its descendants, or owned by the named instance, becomes READY
 * if its goal and all the goal's ancestors are ACTIVE and its owner is
 * RUNNING, and PAUSED otherwise; a task its strategy's decision holds stays
 * PAUSED (ayni_decide_fn). The tasks of a cancelled goal are cancelled with
 * it (ADR-0018). An intent raises the revision of each goal, task and
 * instance it changes by one; others keep theirs. When the intent may bring
 * the goals' due work forward (resume, cancel, stop, a task cancel that
 * frees a place), or creates a goal whose first occurrence or expiry is
 * still to come, the host's wake is called before ayni_submit_intent
 * returns, so the owner steps and learns its next deadline.
 */
ayni_status_t ayni_submit_intent(ayni_kernel_t *kernel, const ayni_intent_v1 *intent,
                                 ayni_intent_result_v1 *out_result, ayni_error_v1 *out_error);

/*
 * Query functions. They write the fields that out_info->struct_size covers
 * and return AYNI_OK; AYNI_INVALID_ARGUMENT for a NULL pointer;
 * AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for
 * out_info->struct_size; AYNI_INVALID_STATE in CONFIGURING and STOPPED;
 * AYNI_NOT_FOUND for an id nothing has. The state they report may not be
 * durable yet: compare its commit_seq with durable_seq (C-COMMIT-004).
 */
ayni_status_t ayni_get_agent(const ayni_kernel_t *kernel, uint64_t agent_id,
                             ayni_agent_info_v1 *out_info);
/* Since 0.13 a terminal goal whose place went to a new goal is NOT_FOUND
 * (ADR-0025). */
ayni_status_t ayni_get_goal(const ayni_kernel_t *kernel, uint64_t goal_id,
                            ayni_goal_info_v1 *out_info);
/* Since 0.6. An ended task whose place went to a new task is NOT_FOUND
 * (ADR-0018). Since 0.13 so is a terminal goal whose place went to a new
 * goal, with its tasks (ADR-0025). */
ayni_status_t ayni_get_task(const ayni_kernel_t *kernel, uint64_t task_id,
                            ayni_task_info_v1 *out_info);

/* ---- Events (since 0.9) ------------------------------------------------
 *
 * Events a host posts from its own threads, other than completions of the
 * kernel's requests (ADR-0015, ADR-0021). They wait in a queue of
 * ingress_capacity events until ayni_step takes them. The kinds this library
 * implements; the other planned kinds are in include/ayni/draft/
 * kernel_api.h. Stop and safety state never travel as events (C-STOP-001).
 */
typedef uint32_t ayni_event_kind_t;

/* A local input, such as a button press or a local rule firing. It is never
 * dropped for room: a full queue refuses it. */
#define AYNI_EVENT_LOCAL_INPUT ((ayni_event_kind_t)3)

typedef struct ayni_event_v1 {
    uint32_t struct_size;
    ayni_event_kind_t kind;
    /* LOCAL_INPUT: a catalog id naming the input, e.g. "button.record". It
     * triggers the goals created with it as their trigger_source. */
    ayni_string_v1 source;
} ayni_event_v1;

/*
 * Queues a copy of the event, then calls wake once the ingress lock is
 * released (ADR-0015). Callable from any thread except an ISR (C-HOST-008),
 * but not from a function the kernel calls, and not while ayni_create or
 * ayni_destroy runs (C-HOST-018). Checks run in this order:
 * - AYNI_INVALID_ARGUMENT for a NULL kernel or event, a struct_size below
 *   sizeof(ayni_event_v1), or kind 0; AYNI_UNSUPPORTED_VERSION for a larger
 *   struct_size;
 * - AYNI_UNSUPPORTED for a kind this library does not implement;
 * - AYNI_INVALID_ARGUMENT for a source that is not a catalog id;
 * - AYNI_INVALID_STATE in CONFIGURING and STOPPED;
 * - Since 0.25, AYNI_CAPACITY_EXCEEDED when this ingress instance has
 *   accepted UINT64_MAX inputs. Restart the process to create a new instance;
 *   an automatic storage reload retains its order and cannot reset the limit;
 * - AYNI_BUSY when ingress_capacity events are queued: nothing is queued,
 *   and the host posts it again after the owner has stepped.
 * Returns AYNI_OK otherwise, in RECOVERING, RUNNING, DRAINING and
 * RESTRICTED (ayni_step says what each does with it). A failed call does
 * not call wake.
 */
ayni_status_t ayni_post_event(ayni_kernel_t *kernel, const ayni_event_v1 *event);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_AYNI_H */
