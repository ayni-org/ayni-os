/* Opaque, boot-local media references. Bytes remain outside Core. Since 0.42. */
#ifndef AYNI_MEDIA_H
#define AYNI_MEDIA_H
#include <ayni/ayni.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Largest bounded Mock media object; actual Host limits may be smaller. Since 0.42. */
#define AYNI_MEDIA_BYTES_MAX 65536u
typedef uint32_t ayni_media_content_t;
/* JPEG image bytes, never interpreted by Core. Since 0.42. */
#define AYNI_MEDIA_JPEG ((ayni_media_content_t)1)
/* Signed 16-bit PCM bytes; format details belong to the Provider. Since 0.42. */
#define AYNI_MEDIA_PCM_S16 ((ayni_media_content_t)2)
/* Opaque test bytes used by M1 Mock fixtures. Since 0.42. */
#define AYNI_MEDIA_MOCK_BYTES ((ayni_media_content_t)3)

/* Complete identity/retention metadata; no pointer, path, URL or credentials.
 * Capture, reading and egress remain separately authorized. Since 0.42. */
typedef struct ayni_media_ref_v1 {
    uint32_t struct_size;              /* Exact sizeof. */
    ayni_media_content_t content_type; /* JPEG, PCM_S16 or MOCK_BYTES. */
    uint64_t media_id;   /* Positive Host identity, not reused in one boot. 0 means no media. */
    uint64_t boot_epoch; /* Kernel boot that owns the reference; positive when media_id != 0. */
    uint64_t owner_instance_id;     /* Positive owning Agent instance; no implicit sharing. */
    uint64_t generation;            /* Positive collection generation, paired with source_id. */
    uint64_t byte_size;             /* 1..MEDIA_BYTES_MAX opaque bytes. */
    uint64_t valid_until_us;        /* Exclusive finite local monotonic deadline. */
    ayni_catalog_name_v1 source_id; /* Exact copied registered source identity. */
    uint32_t source_version;        /* Positive source semantics version. */
    uint32_t reserved;              /* Zero. */
    ayni_catalog_name_v1 purpose;   /* Exact copied catalog token; required when present. */
} ayni_media_ref_v1;

/* Pure intrinsic validation, shared by Core and Host stores. No Kernel access.
 * OK, INVALID_ARGUMENT or UNSUPPORTED_VERSION; does not prove current permission,
 * byte existence, freshness or generation. Since 0.42. */
ayni_status_t ayni_validate_media_reference(const ayni_media_ref_v1 *reference);
#ifdef __cplusplus
}
#endif
#endif
