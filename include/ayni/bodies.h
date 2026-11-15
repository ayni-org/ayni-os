/* Host body store: message bodies kept outside Core records (ADR-0096). Since 0.54.
 * Core, checkpoints and Goal parameters hold only a reference, a length and a
 * digest. The composition root supplies nonblocking ownership/persistence ports;
 * Core may share its already validated normalized result when explicitly configured. */
#ifndef AYNI_BODIES_H
#define AYNI_BODIES_H

#include <ayni/ayni.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One immutable stored body. id is the store's nonzero identity; zero means no
 * body. digest is ayni_body_digest of the complete bytes. Since 0.54. */
typedef struct ayni_body_ref_v1 {
    uint64_t id;       /* Nonzero store identity; zero only for the absent reference. */
    uint64_t digest;   /* ayni_body_digest of exactly size bytes. */
    uint32_t size;     /* Complete byte length; zero is a valid empty body. */
    uint32_t reserved; /* Must be zero. */
} ayni_body_ref_v1;

/* Pure 64-bit FNV-1a digest of the bytes; it tells contents apart but is not
 * a defence against a chosen collision. data may be null only when size is zero. */
uint64_t ayni_body_digest(ayni_bytes_v1 bytes);
/* Pure check that bytes are exactly what ref names: INVALID_ARGUMENT for a
 * malformed reference, DENIED when length or digest differs, otherwise OK. */
ayni_status_t ayni_body_verify(const ayni_body_ref_v1 *ref, ayni_bytes_v1 bytes);

/* All callbacks are nonblocking and may run on any non-ISR thread; a store
 * serializes its own state. scope is a release domain the writer chooses (a
 * conversation, a Task or a Call), never an authority.
 *
 * write copies body. Identical bytes in the same scope name the same body.
 * It returns NOT_READY until the body is durable and immutable, then OK with
 * its reference; a producer hands a reference on only after OK.
 * read copies exactly ref->size bytes into out (capacity >= size). It returns
 * NOT_READY while a load is pending and NOT_FOUND once released.
 * release is idempotent, and the store may delete the body at once. Every
 * releaser calls it only once nothing durable names the body: Core waits until
 * the commit that dropped its last record of the reference is durable. A crash
 * between that commit and the release leaves an unreferenced body behind. */
#define AYNI_BODY_STORE_SHARED_RESULTS 1u /* Core may share normalized results. Since 0.55. */
typedef struct ayni_body_store_v1 {
    uint32_t struct_size; /* sizeof this descriptor. */
    uint32_t reserved;    /* AYNI_BODY_STORE_* options; zero preserves inline Call results. */
    void *context;        /* Caller-owned; lives until every user has stopped. */
    ayni_status_t (*write)(void *context, uint64_t scope, ayni_bytes_v1 body,
                           ayni_body_ref_v1 *out);
    ayni_status_t (*read)(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                          uint32_t capacity);
    void (*release)(void *context, const ayni_body_ref_v1 *ref);
    /* Since 0.55: copy into bounded resident storage and return an immediately
     * readable reference. NOT_READY accepts nothing. A staged body cannot be
     * evicted before persistence; a failure preserves accepted bytes. stage itself
     * schedules no I/O: confirm requests persistence without waiting. Optional:
     * consumers requiring staging reject stores without these three ports. */
    ayni_status_t (*stage)(void *context, uint64_t scope, ayni_bytes_v1 body,
                           ayni_body_ref_v1 *out);
    /* Since 0.55: acquire an independent, idempotent scope reference to the
     * same immutable bytes. Releasing either reference preserves the other.
     * Callers choose distinct scopes for independently released owners. */
    ayni_status_t (*share)(void *context, uint64_t scope, const ayni_body_ref_v1 *source,
                           ayni_body_ref_v1 *out);
    /* Since 0.55: OK only after bytes and ownership are durable; NOT_READY
     * while pending, otherwise the classified save error. Never retries an
     * unknown tool execution. write is stage followed by this confirmation. */
    ayni_status_t (*confirm)(void *context, const ayni_body_ref_v1 *ref);
    /* Since 0.55: optional expiry of an unconfirmed model request after its
     * terminal fact is durable and the Provider has stopped reading. Confirmed
     * owners survive. The Call retains the original reference as metadata; its
     * temporary bytes are no longer readable. Idempotent and nonblocking. */
    void (*release_temporary)(void *context, const ayni_body_ref_v1 *ref);
} ayni_body_store_v1;

#ifdef __cplusplus
}
#endif
#endif
