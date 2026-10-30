/* Since 0.28. Storage-format helpers shared by Core and Hosts (ADR-0040).
 * These inspect untrusted bytes; they do not authorize recovery or a write. */
#ifndef AYNI_STORAGE_FORMAT_H
#define AYNI_STORAGE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Current compiled Core batch/snapshot schema, independent of ABI. Since 0.49.
 * This declaration does not validate media or authorize migration/recovery. */
#define AYNI_STORAGE_SCHEMA_CURRENT 5u

/* Since 0.28. Bytes through transition_id in contracts/storage/records.md.
 * This is a prefix length, not a complete batch/header length. */
#define AYNI_STORAGE_BATCH_IDENTITY_PREFIX_BYTES 32u

/* Since 0.28. Fixed-layout value, not an extensible descriptor. Owns a copy
 * of the three unvalidated identity fields; no pointers or borrowed storage. */
typedef struct {
    uint32_t writer_epoch;  /* Raw writer generation; zero has no special meaning here. */
    uint64_t commit_seq;    /* Raw batch sequence; validity is checked by recovery. */
    uint64_t transition_id; /* Raw transition identity, including zero if encoded. */
} ayni_storage_batch_identity_v1;

/* Since 0.28. Returns 1 when bytes contains an AYJB identity prefix and copies
 * its three little-endian fields to *out; otherwise returns 0, leaving *out
 * unchanged (NULL input/output, short input or a different magic).
 * Reads only the prefix, even if size is larger. Does NOT validate schema,
 * flags, lengths, CRC, commit marker or semantic identity ranges. Callers
 * retain their framing checks; Core must still validate the complete batch.
 * No allocation, I/O or retained references. Any thread; concurrent calls
 * require independently owned output and immutable input during the call. */
int ayni_storage_read_batch_identity(const uint8_t *bytes, size_t size,
                                     ayni_storage_batch_identity_v1 *out);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_STORAGE_FORMAT_H */
