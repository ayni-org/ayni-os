/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols.
 *
 * Planned Host ports beyond the clock in ayni_host_v1: wake, persistent
 * storage, completion slots and the trace sink (contracts/host/
 * host_ports.md). When implemented, these members are appended to
 * ayni_host_v1 in ayni.h (C-VER-002); the separate struct name keeps the
 * draft from colliding with the published one.
 *
 * Every function pointer here is called on the kernel owner thread and must
 * not block or call back into the kernel, except wake, which any thread may
 * call (C-HOST-002, C-HOST-007).
 */
#ifndef AYNI_DRAFT_HOST_V1_H
#define AYNI_DRAFT_HOST_V1_H

#include <ayni/ayni.h>
#include <ayni/draft/types_v1.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Synchronous answer of every submit-like port (C-HOST-004). Only
 * REJECTED_NOT_RETAINED proves that no work exists; ACCEPTED and
 * ACCEPT_UNKNOWN are both in flight, including work queued inside the Host.
 */
typedef uint32_t ayni_submit_result_t;

#define AYNI_SUBMIT_REJECTED_NOT_RETAINED ((ayni_submit_result_t)1)
#define AYNI_SUBMIT_ACCEPTED ((ayni_submit_result_t)2)
#define AYNI_SUBMIT_ACCEPT_UNKNOWN ((ayni_submit_result_t)3)

/*
 * A completion slot reserved by the kernel before it calls a port (C-HOST-006).
 * Owned by the kernel; the Host keeps the pointer until it posts the
 * terminal result (ayni_completion_post here, ayni_provider_post for
 * Provider attempts).
 */
typedef struct ayni_completion ayni_completion_t;

/* Outcomes of storage writes (C-COMMIT-001, C-HOST-010). Distinct from the
 * AYNI_STORAGE_FAILED status code, which reports a failed kernel call. */
#define AYNI_WRITE_OK 1u
#define AYNI_WRITE_FAILED 2u
#define AYNI_WRITE_UNKNOWN 3u

/* Writer fence outcomes (C-HOST-012). */
#define AYNI_FENCE_DRAINED 1u
#define AYNI_FENCE_EPOCH_INSTALLED 2u
#define AYNI_FENCE_UNAVAILABLE 3u

typedef struct ayni_completion_result_v1 {
    uint32_t struct_size;
    uint32_t outcome; /* AYNI_WRITE_* or AYNI_FENCE_*, per port; AYNI_WRITE_OK for reads */
    uint64_t value;   /* bytes read for read ports; 0 otherwise */
} ayni_completion_result_v1;

/*
 * Posts the single result of a reserved completion and wakes the owner
 * thread. Callable from any non-ISR thread; never fails for lack of queue
 * space because the slot was reserved before the request was issued. A
 * second post on the same slot is a Host defect and is ignored (C-CALL-012).
 */
ayni_status_t ayni_completion_post(ayni_completion_t *completion,
                                   const ayni_completion_result_v1 *result);

/* One commit batch, encoded per contracts/storage/records.md. */
typedef struct ayni_storage_batch_v1 {
    uint32_t struct_size;
    uint32_t writer_epoch;
    uint64_t transition_id;
    uint64_t commit_seq;
    ayni_bytes_v1 encoded; /* header, records and commit marker */
} ayni_storage_batch_v1;

/* Storage capability flags (C-HOST-011). */
#define AYNI_STORAGE_ATOMIC_REPLACE 0x1u /* power-safe replace and flush */

/* Trace record handed to the log sink; already desensitized (C-HOST-014). */
typedef struct ayni_trace_record_v1 {
    uint32_t struct_size;
    uint32_t code;
    uint64_t at_us;
    ayni_object_ref_v1 object;
    uint64_t values[2];
} ayni_trace_record_v1;

typedef struct ayni_host_ports_v1 {
    uint32_t struct_size;
    uint32_t storage_flags; /* AYNI_STORAGE_ATOMIC_REPLACE */
    void *ctx;

    /* Any thread, idempotent; schedules ayni_step on the owner thread. */
    void (*wake)(void *ctx);

    /* Durable append; completes with AYNI_WRITE_OK only once the batch
     * survives power loss (C-HOST-010). */
    ayni_submit_result_t (*storage_append)(void *ctx, const ayni_storage_batch_v1 *batch,
                                           ayni_completion_t *completion);

    /* Reads complete committed batches from from_commit_seq on into buffer;
     * the completion value is the number of bytes written. */
    ayni_submit_result_t (*storage_read_prefix)(void *ctx, uint64_t from_commit_seq,
                                                uint8_t *buffer, uint32_t buffer_size,
                                                ayni_completion_t *completion);

    /* Makes the previous writer unable to modify the log (C-COMMIT-010). */
    ayni_submit_result_t (*storage_fence_writers)(void *ctx, uint32_t new_writer_epoch,
                                                  ayni_completion_t *completion);

    /* Snapshot slots 0 and 1; with AYNI_STORAGE_ATOMIC_REPLACE the Host may
     * back both by one replaceable file (C-STORE-006). */
    ayni_submit_result_t (*storage_write_snapshot)(void *ctx, uint32_t slot, ayni_bytes_v1 snapshot,
                                                   ayni_completion_t *completion);
    ayni_submit_result_t (*storage_read_snapshot)(void *ctx, uint32_t slot, uint8_t *buffer,
                                                  uint32_t buffer_size,
                                                  ayni_completion_t *completion);

    /* Synchronous; bytes still writable to the log (C-HOST-013). */
    ayni_status_t (*storage_free_bytes)(void *ctx, uint64_t *out_free_bytes);

    /* Drops and counts when full; never blocks the kernel. */
    void (*log)(void *ctx, const ayni_trace_record_v1 *record);
} ayni_host_ports_v1;

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_HOST_V1_H */
