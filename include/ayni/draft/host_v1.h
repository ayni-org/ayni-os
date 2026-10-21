/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols.
 *
 * Planned trace sink beyond the published clock, ingress and storage
 * ports in ayni_host_v1 (contracts/host/host_ports.md). When implemented,
 * append the sink to ayni_host_v1 under C-VER-002.
 *
 * Every function pointer here is called on the kernel owner thread and must
 * not block or call back into the kernel (C-HOST-002).
 *
 */
#ifndef AYNI_DRAFT_HOST_V1_H
#define AYNI_DRAFT_HOST_V1_H

#include <ayni/ayni.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    void *ctx;

    /* Drops and counts when full; never blocks the kernel. */
    void (*log)(void *ctx, const ayni_trace_record_v1 *record);
} ayni_host_ports_v1;

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_HOST_V1_H */
