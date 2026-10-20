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
#define AYNI_ABI_MINOR 10u

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
    /* Since 0.4. The buffer for log reads during recovery and for one
     * encoded commit batch (ADR-0016); it bounds the largest batch. */
    uint32_t storage_buffer_bytes;
    /* Since 0.5. The work model (ADR-0017). The last two are limits rather
     * than storage, with the same range and default rules. */
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
} ayni_config_v1;

#define AYNI_INGRESS_CAPACITY_DEFAULT 128u
#define AYNI_INGRESS_CAPACITY_MAX 4096u
#define AYNI_EVENT_BODY_MAX_BYTES_DEFAULT 128u
#define AYNI_EVENT_BODY_MAX_BYTES_MAX 4096u
#define AYNI_COMPLETION_SLOT_COUNT_DEFAULT 16u
#define AYNI_COMPLETION_SLOT_COUNT_MAX 1024u
#define AYNI_STORAGE_BUFFER_BYTES_DEFAULT 1024u
#define AYNI_STORAGE_BUFFER_BYTES_MIN 256u
#define AYNI_STORAGE_BUFFER_BYTES_MAX 65536u
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
     * written; 0 means no batch is left. AYNI_WRITE_FAILED when the next
     * batch is larger than buffer_size. The host may write the buffer only
     * until it posts.
     */
    ayni_submit_result_t (*storage_append)(void *ctx, const ayni_storage_batch_v1 *batch,
                                           ayni_attempt_ref_v1 attempt,
                                           ayni_completion_t *completion);
    ayni_submit_result_t (*storage_read_prefix)(void *ctx, uint64_t from_commit_seq,
                                                uint8_t *buffer, uint32_t buffer_size,
                                                ayni_attempt_ref_v1 attempt,
                                                ayni_completion_t *completion);
} ayni_host_v1;

/*
 * Since 0.4. Why a kernel is RESTRICTED (ADR-0016); NONE in every other
 * state. The kernel keeps only read-only diagnostics and shutdown.
 */
typedef uint32_t ayni_restriction_t;

#define AYNI_RESTRICTION_NONE ((ayni_restriction_t)0)
#define AYNI_RESTRICTION_STORAGE_REJECTED ((ayni_restriction_t)1)      /* host refused a request */
#define AYNI_RESTRICTION_STORAGE_READ_FAILED ((ayni_restriction_t)2)   /* log read not reliable */
#define AYNI_RESTRICTION_STORAGE_WRITE_FAILED ((ayni_restriction_t)3)  /* boot epoch not stored */
#define AYNI_RESTRICTION_STORAGE_WRITE_UNKNOWN ((ayni_restriction_t)4) /* may be stored */
#define AYNI_RESTRICTION_LOG_CORRUPT ((ayni_restriction_t)5)           /* C-STORE-009 */
#define AYNI_RESTRICTION_LOG_UNSUPPORTED ((ayni_restriction_t)6)       /* C-STORE-004 */
#define AYNI_RESTRICTION_BOOT_EPOCH_EXHAUSTED ((ayni_restriction_t)7)

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
     * read so far; from RUNNING on, the batch that stored the boot epoch.
     * Transitions applied in RUNNING are not yet written (ADR-0017), so it
     * stays there and never covers them. Compare a commit_seq with it only
     * within the boot that assigned that commit_seq: a later boot assigns
     * the positions above durable_seq again (C-COMMIT-016).
     */
    uint64_t durable_seq;
    /* Since 0.9 (ADR-0021). Events ayni_step took in RUNNING that no goal's
     * trigger took, and events it discarded in DRAINING or RESTRICTED or
     * left queued on entering STOPPED. */
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
     * the end of such an occurrence's wait. */
    uint64_t next_deadline_us;
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
 *   host function, only one of the two storage ports, or memory not aligned
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
 * CONFIGURING, when the host table has no storage ports (C-HOST-015); since
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
 * keeps its lifecycle and never expires (C-COMMIT-012). Since 0.10 it then
 * has the strategies decide the tasks that were READY when the step began,
 * one item and transition each, the task READY longest first (ADR-0022,
 * ayni_register_strategy). Since 0.9 it then takes the events queued when
 * it comes to them, one item each (ADR-0021):
 * in RUNNING each is recorded, in one transition, as an occurrence of every
 * goal whose trigger takes it, due at the step's start, or counted as
 * unmatched; in DRAINING and RESTRICTED each is discarded and counted; in
 * RECOVERING they stay queued. Does at most max_items items and stops once
 * max_time_us has passed on the host clock, checked after each item; never
 * waits on storage or any other I/O (C-LIMIT-011). out_report (nullable)
 * receives the fields its struct_size covers. Returns AYNI_OK;
 * AYNI_INVALID_ARGUMENT if kernel is NULL or max_items or max_time_us is 0;
 * AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for
 * out_report->struct_size; AYNI_INVALID_STATE in CONFIGURING and STOPPED,
 * and when called from a callback.
 */
ayni_status_t ayni_step(ayni_kernel_t *kernel, uint32_t max_items, uint32_t max_time_us,
                        ayni_step_report_v1 *out_report);

/*
 * Enters DRAINING from RECOVERING, RUNNING or RESTRICTED; no new storage
 * request is started after that. It does not wake the owner: the caller goes
 * on calling ayni_step, which enters STOPPED once every reserved completion
 * has been posted and applied (C-HOST-018). Returns
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
/* UNSUPPORTED: a nonzero command_id; command idempotency is not built yet. */
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
/* CAPACITY_EXCEEDED: goal_capacity goals exist, terminal ones included. */
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
 * max_intents_per_call, or the strategy returned AYNI_CAPACITY_EXCEEDED. */
#define AYNI_REASON_POLICY_OUTPUT_LIMIT ((ayni_reason_t)28)
/* INVALID_ARGUMENT: an intent of the decision has another struct_size, or
 * two of its intents would each end the task. */
#define AYNI_REASON_POLICY_INTENT ((ayni_reason_t)29)
/* INVALID_ARGUMENT: the strategy returned AYNI_OK without an intent, which
 * leaves the task with nothing to wait for. */
#define AYNI_REASON_POLICY_NO_PROGRESS ((ayni_reason_t)30)

/* Classified failure detail. Carries a reason and the object it concerns,
 * never credentials, private memory text, media or personal data. */
typedef struct ayni_error_v1 {
    uint32_t struct_size;
    ayni_status_t status; /* the call's return value */
    ayni_reason_t reason;
    uint32_t reserved;         /* written as 0 */
    ayni_object_ref_v1 object; /* all 0 when no object is concerned */
} ayni_error_v1;

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
    uint64_t instance_id; /* 0 until the kernel first enters RUNNING */
    uint64_t commit_seq;  /* of the last change to the instance this boot; 0 before RUNNING */
    uint32_t revision;    /* since 0.6; 1 when the instance starts; 0 before RUNNING */
    uint32_t reserved;    /* written as 0 */
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
 * a due occurrence of a goal that may advance waits for at most its goal
 * type's occurrence_wait_max_us, counted from when it fell due; then the
 * step that finds its wait over skips every occurrence of the goal due by
 * the step's start, counting them and giving the reason (C-RES-005). A goal
 * created with an expiry becomes EXPIRED at that time, paused or not,
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
    uint64_t transition_id; /* that created the goal, this boot */
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
     * AYNI_REASON_TASK_CAPACITY or AYNI_REASON_ID_EXHAUSTED; NONE before any. */
    ayni_reason_t skip_reason;
    /* Since 0.8. While the goal has an occurrence to come, what would keep it
     * from becoming a task now, as skip_reason; AYNI_REASON_NONE when
     * nothing would or none will come. Holding by a pause is in the
     * lifecycles, not here. A LOCAL_INPUT goal has one to come while an
     * occurrence waits. */
    ayni_reason_t wait_reason;
    uint32_t reserved;            /* written as 0 */
    uint32_t trigger_source_size; /* since 0.9; 0 unless the trigger is LOCAL_INPUT */
    char trigger_source[AYNI_CATALOG_ID_MAX_BYTES]; /* since 0.9; as created, not terminated */
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
    uint64_t transition_id; /* that created the task, this boot */
    uint64_t commit_seq;    /* of the last change; durable once durable_seq covers it */
    uint32_t revision;
    uint32_t cancel_revision; /* the revision that cancelled it; 0 if not cancelled */
    /* Since 0.6; since 0.7 the latest of the goal's occurrences it stands
     * for. */
    uint32_t occurrence;
    uint32_t occurrence_count; /* since 0.7; how many it stands for, at least 1 */
    /* Since 0.7. When the latest occurrence it stands for fell due, on the
     * host monotonic clock. */
    uint64_t due_us;
    /* Since 0.10 (ADR-0022). Why its strategy's decision holds the task
     * PAUSED: the status and reason of the hold, the status being the one
     * the strategy returned or the one the kernel refused its decision
     * with. A held task stays PAUSED while its goal and owner may advance,
     * until it ends. AYNI_OK and AYNI_REASON_NONE for a task never held. */
    ayni_status_t hold_status;
    ayni_reason_t hold_reason;
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

/*
 * One envelope for every intent kind. The principal and channel are the
 * session's, set when the host opened it (C-AUTH-010); an intent cannot
 * claim others.
 */
typedef struct ayni_intent_v1 {
    uint32_t struct_size;
    ayni_intent_kind_t kind;
    /* Management command idempotency key (C-WIRE-003); must be 0 until the
     * kernel keeps command mappings (MA-010). */
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

typedef struct ayni_intent_result_v1 {
    uint32_t struct_size;
    uint32_t reserved;         /* written as 0 */
    ayni_object_ref_v1 object; /* the goal, with its revision after the intent */
    uint64_t commit_seq;       /* of the transition the intent applied, this boot */
} ayni_intent_result_v1;

/*
 * Registers the owner principal, whom the host has bound and authenticated.
 * Returns AYNI_OK; AYNI_INVALID_ARGUMENT for a NULL pointer, a kind other
 * than AYNI_PRINCIPAL_OWNER, a zero principal_id or binding_revision, or a
 * nonzero reserved field; AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION
 * for struct_size; AYNI_INVALID_STATE outside CONFIGURING;
 * AYNI_CAPACITY_EXCEEDED once an owner is registered, since a device has
 * exactly one (C-PROD-001).
 */
ayni_status_t ayni_register_principal(ayni_kernel_t *kernel,
                                      const ayni_principal_desc_v1 *principal);

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
 * kernel provides. decide must not call kernel functions, block or perform
 * I/O (C-POLICY-001). Given the same snapshot it gives the same decision,
 * reading no clock or state the snapshot does not hold (C-POLICY-006). The
 * shape serves deterministic replay and replacing strategies; it is not a
 * security boundary.
 */

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
} ayni_task_snapshot_v1;

/* The intent kinds this library implements; the other planned kinds are in
 * include/ayni/draft/policy_v1.h. Both end the task. */
typedef uint32_t ayni_policy_intent_kind_t;

#define AYNI_POLICY_REPORT_COMPLETION ((ayni_policy_intent_kind_t)5) /* the task is done */
#define AYNI_POLICY_REPORT_FAILURE ((ayni_policy_intent_kind_t)6)    /* it cannot be done */

typedef struct ayni_policy_intent_v1 {
    uint32_t struct_size;
    ayni_policy_intent_kind_t kind;
    uint32_t expected_revision; /* the snapshot's revision (C-COMMIT-012) */
} ayni_policy_intent_v1;

/* Where decide writes its decision. The kernel provides intents and sets
 * intent_capacity to the strategy's max_intents_per_call; decide writes
 * intents[0] to intents[intent_count - 1] and sets intent_count. The kernel
 * reads back only intent_count and its own array. */
typedef struct ayni_policy_output_v1 {
    uint32_t struct_size;
    uint32_t intent_capacity;
    uint32_t intent_count;
    uint32_t reserved; /* written as 0 */
    ayni_policy_intent_v1 *intents;
} ayni_policy_output_v1;

/*
 * Decides one task: returns AYNI_OK with the decision in output,
 * AYNI_CAPACITY_EXCEEDED when the decision needs more than intent_capacity
 * intents (C-POLICY-002), or any other failure status when it cannot
 * decide.
 *
 * The kernel applies the decision as one transition. One intent that ends
 * the task makes it COMPLETED or FAILED. If its goal then has no other task
 * that has not ended and no occurrence to come, as a ONCE or DELAY goal
 * after its one task, the goal becomes SATISFIED or FAILED likewise, and its
 * descendants that are not terminal are CANCELLED with their tasks. Any
 * other decision holds the task: it becomes PAUSED with the status and
 * reason in ayni_task_info_v1, and stays PAUSED until it is cancelled or
 * expires (C-POLICY-002, C-POLICY-003). It is held when decide fails, with
 * the status decide returned and AYNI_REASON_POLICY_OUTPUT_LIMIT for
 * AYNI_CAPACITY_EXCEEDED, AYNI_REASON_NONE otherwise; with
 * CAPACITY_EXCEEDED and POLICY_OUTPUT_LIMIT when intent_count exceeds
 * intent_capacity; with INVALID_ARGUMENT and POLICY_NO_PROGRESS for
 * AYNI_OK without an intent; then, for each intent in turn, with
 * INVALID_ARGUMENT and POLICY_INTENT for another struct_size, UNSUPPORTED
 * and INTENT_UNSUPPORTED for a kind this library does not implement,
 * REVISION_CONFLICT and TASK_REVISION for another expected_revision, and
 * INVALID_ARGUMENT and POLICY_INTENT when an earlier intent already ends
 * the task; and with CAPACITY_EXCEEDED and REVISION_EXHAUSTED when ending
 * the task would raise a revision past the largest (C-COMMIT-012).
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
    uint32_t reserved;             /* must be 0 */
} ayni_strategy_v1;

/*
 * Registers a strategy. One version of an id is registered at a time: goal
 * types name strategies by id, and a strategy is replaced only across a
 * restart (C-POLICY-010). ayni_step has it decide each task of those goal
 * types that was READY when the step began, so each such task once per
 * step at most, and none at the largest revision. Returns AYNI_OK;
 * AYNI_INVALID_ARGUMENT for a NULL pointer or decide, a zero version, a
 * strategy_id that is not a catalog id, a max_intents_per_call of 0 or
 * above policy_intent_capacity, a nonzero reserved field, or a strategy_id
 * already registered; AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for
 * struct_size; AYNI_INVALID_STATE outside CONFIGURING;
 * AYNI_CAPACITY_EXCEEDED once strategy_capacity strategies are registered.
 */
ayni_status_t ayni_register_strategy(ayni_kernel_t *kernel, const ayni_strategy_v1 *strategy);

/*
 * Opens a session for a principal on a channel the host has authenticated
 * (C-AUTH-010). Sessions are not stored: a reboot closes them all. On
 * AYNI_OK, *out_session_id receives a new id, never 0. Returns AYNI_OK;
 * AYNI_INVALID_ARGUMENT for a NULL pointer or an unknown channel;
 * AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for struct_size;
 * AYNI_INVALID_STATE when called from a callback; AYNI_NOT_READY outside
 * RUNNING; AYNI_NOT_FOUND for an unregistered principal;
 * AYNI_CAPACITY_EXCEEDED once session_capacity sessions are open or no id is
 * left this boot (C-ID-010).
 */
ayni_status_t ayni_open_session(ayni_kernel_t *kernel, const ayni_session_desc_v1 *session,
                                uint64_t *out_session_id);

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
 * durable_seq reaches the commit_seq in out_result (C-COMMIT-004), which
 * does not happen yet (ADR-0017). out_result (nullable) is written on AYNI_OK
 * only. out_error (nullable) is written on every return once its own
 * struct_size is accepted, with status AYNI_OK and no reason on success.
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
 * 2. AYNI_UNSUPPORTED: an intent kind this library does not implement, or a
 *    nonzero command_id.
 * 3. AYNI_INVALID_STATE when called from a callback; then
 *    AYNI_NOT_READY outside RUNNING, without queueing (C-LIFE-004).
 * 4. AYNI_DENIED: session_id names no open session. Until grants exist, a
 *    session of the owner may submit every intent (ADR-0017).
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
 * id and the commit_seq reserved for it are used up.
 *
 * GOAL_CREATE creates an ACTIVE goal at revision 1. GOAL_PAUSE takes an
 * ACTIVE goal to PAUSED and GOAL_RESUME takes it back; neither changes its
 * children. GOAL_CANCEL takes a goal that is not yet terminal, and every
 * descendant that is not yet terminal, to CANCELLED in one transition
 * (C-LIFE-022). TASK_CANCEL takes a READY or PAUSED task to CANCELLED.
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
ayni_status_t ayni_get_goal(const ayni_kernel_t *kernel, uint64_t goal_id,
                            ayni_goal_info_v1 *out_info);
/* Since 0.6. An ended task whose place went to a new task is NOT_FOUND
 * (ADR-0018). */
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
