/* Caller-owned temporary work memory; no state, allocation or platform selection.
 * Since 0.53. The byte region is borrowed only for a synchronous operation. */
#ifndef AYNI_WORKSPACE_H
#define AYNI_WORKSPACE_H

#include <ayni/ayni.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One exclusive temporary region. Independent regions allow concurrent calls;
 * a callback must use another region while its caller still borrows this one.
 * Region, immutable inputs and outputs must not overlap. No pointer into this
 * region may escape the synchronous operation or its receive callback. */
typedef struct ayni_workspace_v1 {
    uint32_t struct_size; /* sizeof this descriptor; exact compiled layout. */
    uint32_t reserved;    /* Zero; no flags or persistent state. */
    void *data;           /* Caller-owned aligned region; nonnull for any operation. */
    size_t byte_capacity; /* Available bytes; at least the queried requirement. */
} ayni_workspace_v1;

/* Required bytes/alignment for schema/value/argument synchronous operations.
 * Both outputs are required; failure preserves them. Since 0.53. */
ayni_status_t ayni_value_workspace_requirements(size_t *out_bytes, size_t *out_alignment);
/* Required bytes/alignment for complete original-business proof inspection.
 * The receive callback's own workspace is additional. Since 0.53. */
ayni_status_t ayni_business_workspace_requirements(size_t *out_bytes, size_t *out_alignment);

#ifdef __cplusplus
}
#endif
#endif
