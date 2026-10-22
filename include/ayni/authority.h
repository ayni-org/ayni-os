/* Since ABI 0.17: finite Capability authorization fields. Registration
 * describes a capability; it does not grant access or implement a call.
 * All input views are borrowed only until the registration call returns.
 */
#ifndef AYNI_AUTHORITY_H
#define AYNI_AUTHORITY_H

#include <ayni/ayni.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AYNI_AUTHORITY_FIELD_COUNT_MAX 8u
#define AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX 8u

typedef uint32_t ayni_authority_value_kind_t;
#define AYNI_AUTHORITY_I64 ((ayni_authority_value_kind_t)1)
#define AYNI_AUTHORITY_ENUM ((ayni_authority_value_kind_t)2)
#define AYNI_AUTHORITY_IDENTIFIER ((ayni_authority_value_kind_t)3)

typedef uint32_t ayni_authority_field_usage_t;
#define AYNI_AUTHORITY_PARAMETER ((ayni_authority_field_usage_t)1)
#define AYNI_AUTHORITY_EXECUTION_TARGET ((ayni_authority_field_usage_t)2)
#define AYNI_AUTHORITY_EGRESS_DESTINATION ((ayni_authority_field_usage_t)3)

typedef struct ayni_authority_i64_range_v1 {
    int64_t min_inclusive;
    int64_t max_inclusive;
} ayni_authority_i64_range_v1;

typedef struct ayni_authority_token_set_v1 {
    const ayni_string_v1 *members;
    uint32_t member_count;
    uint32_t reserved; /* 0; the input count, before deduplication, is 1..8 */
} ayni_authority_token_set_v1;

typedef union ayni_authority_values_v1 {
    ayni_authority_i64_range_v1 integer;
    ayni_authority_token_set_v1 tokens;
} ayni_authority_values_v1;

typedef struct ayni_authority_value_set_v1 {
    uint32_t struct_size;
    ayni_authority_value_kind_t kind;
    /* I64: a nonempty CatalogId (e.g. count, ms, scalar). ENUM and
     * IDENTIFIER: empty. Units match exactly; Core does not convert them. */
    ayni_string_v1 unit;
    ayni_authority_values_v1 values; /* only the arm selected by kind is read */
} ayni_authority_value_set_v1;

typedef struct ayni_authority_field_desc_v1 {
    uint32_t struct_size;
    uint32_t field_id; /* nonzero, unique and stable within this capability version */
    ayni_authority_field_usage_t usage;
    uint32_t reserved; /* 0 */
    ayni_authority_value_set_v1 domain;
} ayni_authority_field_desc_v1;

typedef struct ayni_capability_desc_v1 {
    uint32_t struct_size;
    uint32_t version; /* nonzero; exact ID/version is the registered identity */
    ayni_string_v1 capability_id;
    uint32_t field_count; /* 0..8; fields may be NULL only when count is 0 */
    uint32_t reserved;    /* 0 */
    const ayni_authority_field_desc_v1 *fields;
} ayni_capability_desc_v1;

typedef struct ayni_scope_field_v1 {
    uint32_t struct_size;
    uint32_t field_id;
    ayni_authority_value_set_v1 allowed;
} ayni_scope_field_v1;

typedef struct ayni_scope_v1 {
    uint32_t struct_size;
    uint32_t field_count;
    /* Scope omission means the registered field's whole domain. A concrete
     * request must instead supply every field with one value each. */
    const ayni_scope_field_v1 *fields;
} ayni_scope_v1;

/* Owner thread, outside callbacks, CONFIGURING only. Copies all inputs on
 * success. Requires valid nested struct_size, CatalogIds, finite domains,
 * unique field IDs and at most one IDENTIFIER execution target and egress
 * destination. Returns INVALID_ARGUMENT for malformed or duplicate input,
 * UNSUPPORTED_VERSION for larger structs, CAPACITY_EXCEEDED for a full
 * registry or a complete domain beyond authority_scope_max_bytes. */
ayni_status_t ayni_register_capability(ayni_kernel_t *kernel,
                                       const ayni_capability_desc_v1 *capability);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_AUTHORITY_H */
