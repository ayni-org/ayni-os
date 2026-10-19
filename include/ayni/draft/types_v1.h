/*
 * DRAFT, not part of the published ABI (contracts/domain/versions.md
 * C-VER-007). Declarations here have no symbols; they are compiled by the
 * header checks so that the contract stays expressible in C99 and C++17.
 *
 * Common value types shared by the draft kernel, host, provider, policy and
 * control interfaces.
 *
 * Conventions for every draft struct and enumeration:
 * - Enumerations are uint32_t typedefs with #define values; 0 is never a
 *   valid value, so a zero-initialized field is rejected rather than read as
 *   a default.
 * - Strings are UTF-8 views with an explicit size and no terminator. The
 *   kernel copies them during the call that receives them and checks the
 *   size against the configured limit (contracts/domain/limits.md).
 * - 64-bit identifiers are plain integers here; only their JSON form is a
 *   decimal string (contracts/wire/json_subset.md C-JSON-004).
 */
#ifndef AYNI_DRAFT_TYPES_V1_H
#define AYNI_DRAFT_TYPES_V1_H

#include <ayni/ayni.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ayni_string_v1 {
    const char *data; /* may be NULL only when size is 0 */
    uint32_t size;    /* bytes, not characters */
} ayni_string_v1;

typedef struct ayni_bytes_v1 {
    const uint8_t *data;
    uint32_t size;
} ayni_bytes_v1;

/* Object kinds an ayni_object_ref_v1 or ayni_error_v1 can name. */
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

/*
 * Reference to a versioned object (plan section 3.6). A reference carries no
 * permission: the receiver resolves it against the authenticated source and
 * checks read, transfer and purpose rights (C-WIRE-010).
 */
typedef struct ayni_object_ref_v1 {
    uint64_t origin_device_id;
    uint64_t id;
    uint32_t revision;
    ayni_object_kind_t kind;
} ayni_object_ref_v1;

/*
 * Classified failure detail (C-STATUS-006). Carries a reason code and the
 * object it concerns, never credentials, private memory text, media or
 * personal data. Reason codes are listed per operation in contracts/ and
 * fixed in M1 (MA-005).
 */
typedef struct ayni_error_v1 {
    uint32_t struct_size;
    ayni_status_t status;
    uint32_t reason;
    uint32_t reserved; /* 0 */
    ayni_object_ref_v1 object;
} ayni_error_v1;

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_TYPES_V1_H */
