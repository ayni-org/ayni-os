/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols.
 *
 * Planned Host ports beyond those in ayni_host_v1 (clock, wake, ingress
 * lock, log append and prefix read): writer fencing, snapshots, free space
 * and the trace sink (contracts/host/host_ports.md). When implemented, these members are appended
 * to ayni_host_v1 in ayni.h (C-VER-002); the separate struct name keeps the draft from colliding
 * with the published one.
 *
 * Every function pointer here is called on the kernel owner thread and must
 * not block or call back into the kernel (C-HOST-002).
 *
 * Asynchronous ports (C-HOST-018, C-HOST-019): the kernel reserves a
 * completion (ayni_completion_t in ayni.h) and names the attempt before each
 * call. When the port returns ACCEPTED or ACCEPT_UNKNOWN, the Host owes
 * exactly one ayni_completion_post for that attempt, even when the outcome is
 * unknown, and until it posts it may keep the completion pointer and the
 * request's kernel-owned buffers. After the post it touches neither. The
 * kernel reaches STOPPED only after every reserved completion has been
 * posted, so ayni_destroy can never free memory a Host still points into.
 */
#ifndef AYNI_DRAFT_HOST_V1_H
#define AYNI_DRAFT_HOST_V1_H

#include <ayni/ayni.h>
#include <ayni/draft/types_v1.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writer fence outcomes (C-HOST-012). */
#define AYNI_FENCE_DRAINED 1u
#define AYNI_FENCE_EPOCH_INSTALLED 2u
#define AYNI_FENCE_UNAVAILABLE 3u

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

    /* Makes the previous writer unable to modify the log (C-COMMIT-010). */
    ayni_submit_result_t (*storage_fence_writers)(void *ctx, uint32_t new_writer_epoch,
                                                  ayni_attempt_ref_v1 attempt,
                                                  ayni_completion_t *completion);

    /* Snapshot slots 0 and 1; with AYNI_STORAGE_ATOMIC_REPLACE the Host may
     * back both by one replaceable file (C-STORE-006). snapshot.data and the
     * read buffer follow the same ownership rule as the batch and prefix
     * buffers of ayni_host_v1. Outcomes are AYNI_WRITE_* from ayni.h. */
    ayni_submit_result_t (*storage_write_snapshot)(void *ctx, uint32_t slot, ayni_bytes_v1 snapshot,
                                                   ayni_attempt_ref_v1 attempt,
                                                   ayni_completion_t *completion);
    ayni_submit_result_t (*storage_read_snapshot)(void *ctx, uint32_t slot, uint8_t *buffer,
                                                  uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
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
