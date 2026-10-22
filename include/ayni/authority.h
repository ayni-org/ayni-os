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

/* Since ABI 0.18. A finite Grant expires at its creation boot's deadline;
 * restarting never renews it. PERMANENT is an explicit management choice. */
typedef uint32_t ayni_grant_lifetime_t;
#define AYNI_GRANT_PERMANENT ((ayni_grant_lifetime_t)1)
#define AYNI_GRANT_CURRENT_BOOT ((ayni_grant_lifetime_t)2)
#define AYNI_GRANT_ALLOW_DELEGATION 0x1u
#define AYNI_GRANT_REQUIRES_SCENE 0x2u /* recognized, currently UNSUPPORTED */
#define AYNI_GRANT_REQUIRES_QUOTA 0x4u /* recognized, currently UNSUPPORTED */

typedef struct ayni_grant_create_v1 {
    uint32_t struct_size;
    uint32_t capability_version;
    ayni_string_v1 capability_id;
    uint64_t subject_instance_id;
    uint64_t parent_grant_id; /* 0: root, whose expected_revision must be 0 */
    ayni_grant_lifetime_t lifetime;
    uint32_t flags;
    uint64_t duration_us; /* PERMANENT: 0; CURRENT_BOOT: positive */
    ayni_scope_v1 scope;
} ayni_grant_create_v1;

/* Since 0.19. First acceptance requires a nonzero command ID and current
 * expected_revision. Immediate denial precedes storage admission. Pending
 * control remains accepted through this Kernel's automatic reloads. */
typedef struct ayni_grant_revoke_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* 0 */
    uint64_t grant_id;
} ayni_grant_revoke_v1;

typedef uint32_t ayni_grant_lifecycle_t;
#define AYNI_GRANT_ACTIVE ((ayni_grant_lifecycle_t)1)
#define AYNI_GRANT_EXPIRED ((ayni_grant_lifecycle_t)2)
#define AYNI_GRANT_REVOKED ((ayni_grant_lifecycle_t)3) /* Since 0.19, irreversible */

typedef struct ayni_grant_info_v1 {
    uint32_t struct_size;
    ayni_grant_lifecycle_t
        lifecycle; /* persistent fact; may await durability, not an expiry overlay */
    uint64_t grant_id;
    uint64_t subject_instance_id;
    uint64_t parent_grant_id;
    uint64_t owner_principal_id;
    uint32_t owner_binding_revision;
    uint32_t capability_version;
    char capability_id[AYNI_CATALOG_ID_MAX_BYTES];
    uint32_t capability_id_size;
    uint32_t field_count;
    uint32_t allow_delegation;
    uint32_t depth; /* edges from the root, whose depth is 0 */
    ayni_grant_lifetime_t lifetime;
    uint32_t revision;
    uint64_t duration_us;
    uint64_t deadline_us; /* 0 for PERMANENT or an unusable restored deadline */
    uint64_t created_transition_id;
    uint64_t created_commit_seq;
    uint64_t transition_id;
    uint64_t commit_seq;
    ayni_reason_t effective_reason; /* includes ancestor/hold/expiry/durability checks */
    uint32_t reserved;              /* 0 */
    /* Since 0.19. Root cleanup uses a fixed watermark. The terminal
     * commit_seq retains the original reclamation order. */
    uint64_t revoked_by_root_id;
    uint64_t cleanup_after_grant_id;
    uint64_t cleanup_through_grant_id;
    uint64_t cleanup_transition_id;
    uint64_t cleanup_commit_seq;
} ayni_grant_info_v1;

typedef struct ayni_grant_tokens_info_v1 {
    char members[AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX][AYNI_CATALOG_ID_MAX_BYTES];
    uint32_t member_sizes[AYNI_AUTHORITY_SET_MEMBER_COUNT_MAX];
    uint32_t member_count;
    uint32_t reserved; /* 0 */
} ayni_grant_tokens_info_v1;

typedef union ayni_grant_field_values_v1 {
    ayni_authority_i64_range_v1 integer;
    ayni_grant_tokens_info_v1 tokens;
} ayni_grant_field_values_v1;

typedef struct ayni_grant_field_info_v1 {
    uint32_t struct_size;
    uint32_t field_id;
    ayni_authority_value_kind_t kind;
    ayni_authority_field_usage_t usage;
    char unit[AYNI_CATALOG_ID_MAX_BYTES];
    uint32_t unit_size;
    uint32_t reserved;                 /* 0 */
    ayni_grant_field_values_v1 values; /* only the arm selected by kind is meaningful */
} ayni_grant_field_info_v1;

/* Owner-thread diagnostic queries, RECOVERING through DRAINING. Output is
 * caller-owned and copied as a whole only on success. Unknown Grant/field
 * index returns NOT_FOUND; a reclaimed old reference remains NOT_FOUND.
 * Strings have explicit byte lengths, without an added terminator. */
ayni_status_t ayni_get_grant(const ayni_kernel_t *kernel, uint64_t grant_id,
                             ayni_grant_info_v1 *out_info);
ayni_status_t ayni_get_grant_field(const ayni_kernel_t *kernel, uint64_t grant_id,
                                   uint32_t field_index, ayni_grant_field_info_v1 *out_field);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_AUTHORITY_H */
