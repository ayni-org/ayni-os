/* Host body store: message bodies kept outside Core records (ADR-0096). Since 0.54.
 * Core, checkpoints and Goal parameters hold only a reference, a length and a
 * digest. A store is supplied by the composition root; Core itself only releases. */
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
typedef struct ayni_body_store_v1 {
    uint32_t struct_size; /* sizeof this descriptor. */
    uint32_t reserved;    /* Zero. */
    void *context;        /* Caller-owned; lives until every user has stopped. */
    ayni_status_t (*write)(void *context, uint64_t scope, ayni_bytes_v1 body,
                           ayni_body_ref_v1 *out);
    ayni_status_t (*read)(void *context, const ayni_body_ref_v1 *ref, uint8_t *out,
                          uint32_t capacity);
    void (*release)(void *context, const ayni_body_ref_v1 *ref);
} ayni_body_store_v1;

#ifdef __cplusplus
}
#endif
#endif
