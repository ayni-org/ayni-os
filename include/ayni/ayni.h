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
#define AYNI_ABI_MINOR 4u

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
    uint32_t ingress_capacity;      /* events queued by ayni_post_event */
    uint32_t event_body_max_bytes;  /* largest event body accepted */
    uint32_t completion_slot_count; /* asynchronous Host requests in flight */
    /* Since 0.4. The buffer for log reads during recovery and for one
     * encoded commit batch (ADR-0016); it bounds the largest batch. */
    uint32_t storage_buffer_bytes;
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

/* Correlates an asynchronous attempt with its result (C-ID-006). */
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
} ayni_kernel_info_v1;

/* Since 0.4. What one ayni_step did. */
typedef struct ayni_step_report_v1 {
    uint32_t struct_size;
    uint32_t items_done;
    uint32_t more_ready; /* nonzero: call ayni_step again without waiting for a wake */
    uint32_t reserved;   /* written as 0 */
    /* Host monotonic time by which to call ayni_step even if nothing wakes
     * the owner; UINT64_MAX: none. */
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
 * Writes the fields of out_info that out_info->struct_size covers. Callable in
 * every lifecycle state. Returns AYNI_OK; AYNI_INVALID_ARGUMENT for a NULL
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
 * CONFIGURING, when the host table has no storage ports (C-HOST-015).
 */
ayni_status_t ayni_start(ayni_kernel_t *kernel);

/*
 * Applies posted completions and advances recovery and shutdown. Does at most
 * max_items items and stops once max_time_us has passed on the host clock,
 * checked after each item; never waits on storage or any other I/O
 * (C-LIMIT-011). out_report (nullable) receives the fields its struct_size
 * covers. Returns AYNI_OK; AYNI_INVALID_ARGUMENT if kernel is NULL or
 * max_items or max_time_us is 0; AYNI_INVALID_ARGUMENT or
 * AYNI_UNSUPPORTED_VERSION for out_report->struct_size; AYNI_INVALID_STATE in
 * CONFIGURING and STOPPED.
 */
ayni_status_t ayni_step(ayni_kernel_t *kernel, uint32_t max_items, uint32_t max_time_us,
                        ayni_step_report_v1 *out_report);

/*
 * Enters DRAINING from RECOVERING, RUNNING or RESTRICTED; no new storage
 * request is started after that. Later ayni_step calls enter STOPPED once
 * every reserved completion has been posted and applied (C-HOST-018). Returns
 * AYNI_OK, also in DRAINING and STOPPED; AYNI_INVALID_ARGUMENT if kernel is
 * NULL; AYNI_INVALID_STATE in CONFIGURING, where ayni_destroy applies
 * directly.
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

#ifdef __cplusplus
}
#endif

#endif /* AYNI_AYNI_H */
