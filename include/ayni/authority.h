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
    uint32_t member_count; /* Input count before deduplication: 1..8. */
    uint32_t reserved;     /* 0 */
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

/* Since 0.21. One requirement emitted in declared order. Scope omission
 * expands to this exact capability version's domain; views live through emit. */
typedef struct ayni_goal_requirement_v1 {
    uint32_t struct_size;         /* sizeof this descriptor */
    uint32_t capability_version;  /* nonzero exact registered version */
    ayni_string_v1 capability_id; /* nonempty exact registered identity */
    ayni_scope_v1 scope;          /* requested ceiling, never a concrete action */
} ayni_goal_requirement_v1;

/* Since 0.21. Kernel-owned sink valid only inside export_requirements. Each
 * emit copies input immediately. First failure is sticky even if ignored by
 * the callback; emit is the only allowed operation on this borrowed ctx. */
typedef struct ayni_goal_requirement_sink_v1 {
    uint32_t struct_size; /* sizeof this sink; immutable */
    void *ctx;            /* borrowed Kernel staging context, never retained */
    ayni_status_t (*emit)(void *ctx, const ayni_goal_requirement_v1 *requirement);
} ayni_goal_requirement_sink_v1;

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

/* Since 0.23. Management-only atomic approval of one PROPOSED Goal. Requires
 * a nonzero command ID and expected revision. Echo the immutable creation
 * inputs; the missing mask must name exactly the original unselected items.
 * Existing references are retained and rechecked. New Grants are exclusive
 * nondelegable roots. PERMANENT is the default management choice (duration 0);
 * CURRENT_BOOT explicitly selects a positive, shorter duration in microseconds.
 * Caller owns all nested views through ayni_submit_intent; nothing is retained. */
typedef struct ayni_goal_approve_v1 {
    uint32_t struct_size;           /* sizeof this descriptor */
    uint32_t missing_mask;          /* bit i names requirement i; nonzero, no extra bits */
    uint64_t goal_id;               /* exact retained proposal, never a slot index */
    ayni_grant_lifetime_t lifetime; /* PERMANENT or CURRENT_BOOT */
    uint32_t reserved;              /* 0 */
    uint64_t duration_us;           /* 0 iff PERMANENT; counted from acceptance */
    ayni_goal_create_v1 original;   /* complete original inputs, including original expiry */
} ayni_goal_approve_v1;

/* Since 0.23. Copy original Goal parameters on the owner thread, outside
 * callbacks, RECOVERING through DRAINING. On BUFFER_TOO_SMALL only out_size
 * changes to the required byte count; other errors change no output. A null
 * buffer is valid only with capacity 0. Combine with ayni_get_goal's immutable
 * fields to construct an exact approval request. No exporter is invoked. */
ayni_status_t ayni_get_goal_parameters(const ayni_kernel_t *kernel, uint64_t goal_id,
                                       uint8_t *buffer, uint32_t capacity, uint32_t *out_size);

typedef uint32_t ayni_grant_lifecycle_t;
#define AYNI_GRANT_ACTIVE ((ayni_grant_lifecycle_t)1)
#define AYNI_GRANT_EXPIRED ((ayni_grant_lifecycle_t)2)
#define AYNI_GRANT_REVOKED ((ayni_grant_lifecycle_t)3) /* Since 0.19, irreversible */
/* Since 0.25. Goal-specific authority ended with its bound Goal; irreversible. */
#define AYNI_GRANT_GOAL_ENDED ((ayni_grant_lifecycle_t)4)

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
    /* Since 0.22: 0 for generic authority; otherwise only this live Goal may
     * consume the Grant. A bound Grant is a root and cannot be delegated. */
    uint64_t bound_goal_id;
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

/* Since 0.21. Persisted authority evidence; current usability is separate. */
typedef uint32_t ayni_goal_authority_phase_t;
/* Since 0.21. No saved requirement evidence; never equivalent to zero needs. */
#define AYNI_GOAL_AUTHORITY_UNKNOWN ((ayni_goal_authority_phase_t)0)
/* Since 0.21. At least one requirement has no selected Grant. */
#define AYNI_GOAL_AUTHORITY_PROPOSED ((ayni_goal_authority_phase_t)1)
/* Since 0.21. Complete selected references; consumption rechecks usability. */
#define AYNI_GOAL_AUTHORITY_AUTHORIZED ((ayni_goal_authority_phase_t)2)
/* Since 0.21. Explicit authorization resume is required; user pause is separate. */
#define AYNI_GOAL_AUTHORITY_PAUSED ((ayni_goal_authority_phase_t)3)

/* Since 0.21. Caller-owned summary, including explicit zero versus absence. */
typedef struct ayni_goal_authority_info_v1 {
    uint32_t struct_size;              /* sizeof this output */
    ayni_goal_authority_phase_t phase; /* UNKNOWN iff metadata is absent */
    uint64_t goal_id;                  /* exact owning Goal ID, nonzero */
    uint64_t transition_id;            /* metadata's last update, 0 iff absent */
    uint64_t commit_seq;               /* same update, 0 iff absent */
    uint32_t requirement_count;        /* saved entries, 0 may be explicit */
    uint32_t missing_count;            /* entries without a currently usable reference */
    ayni_reason_t effective_reason;    /* NONE only when currently authorized */
    uint32_t reserved;                 /* 0 */
    /* Since 0.24. Binding log sequence; 0 before admission or when absent. */
    uint64_t binding_commit_seq;
    /* Since 0.24. Admitted parent's binding sequence; 0 for roots/proposals. */
    uint64_t parent_binding_commit_seq;
} ayni_goal_authority_info_v1;

/* Since 0.21. Copy of one immutable requirement and its selected dependency. */
typedef struct ayni_goal_requirement_info_v1 {
    uint32_t struct_size;                          /* sizeof this output */
    uint32_t capability_version;                   /* saved exact nonzero schema version */
    uint64_t grant_id;                             /* selected identity, 0 iff revision is 0 */
    uint32_t grant_revision;                       /* selection revision, not a live-state mirror */
    uint32_t capability_id_size;                   /* used bytes in capability_id, no terminator */
    char capability_id[AYNI_CATALOG_ID_MAX_BYTES]; /* immutable saved identity */
    uint32_t field_count;           /* fields queryable in original domain and Scope */
    ayni_reason_t effective_reason; /* NONE iff selected dependency is usable now */
} ayni_goal_requirement_info_v1;

/* Since 0.21. Both value sets are copied; no borrowed pointers escape. */
typedef struct ayni_goal_requirement_field_info_v1 {
    uint32_t struct_size;            /* sizeof this output */
    ayni_grant_field_info_v1 domain; /* original complete Capability field */
    ayni_grant_field_info_v1 scope;  /* immutable requested subset of domain */
} ayni_goal_requirement_field_info_v1;

/* Since 0.21. Owner-thread queries in the same lifecycle states as get_goal.
 * Indexes use the original export order and canonical field order. Every
 * failure leaves all output bytes unchanged; missing Goal/index is NOT_FOUND.
 * A present Goal with absent metadata returns UNKNOWN and no requirements. */
ayni_status_t ayni_get_goal_authority(const ayni_kernel_t *kernel, uint64_t goal_id,
                                      ayni_goal_authority_info_v1 *out_info);
ayni_status_t ayni_get_goal_requirement(const ayni_kernel_t *kernel, uint64_t goal_id,
                                        uint32_t index, ayni_goal_requirement_info_v1 *out_info);
ayni_status_t ayni_get_goal_requirement_field(const ayni_kernel_t *kernel, uint64_t goal_id,
                                              uint32_t requirement_index, uint32_t field_index,
                                              ayni_goal_requirement_field_info_v1 *out_info);

/* Since 0.24. Persisted Task authority phase, independent of policy holds. */
typedef uint32_t ayni_task_authority_phase_t;
/* Since 0.24. No admission has been restored yet; never means zero needs. */
#define AYNI_TASK_AUTHORITY_UNKNOWN ((ayni_task_authority_phase_t)0)
/* Since 0.24. Complete inherited selections; consumption rechecks validity. */
#define AYNI_TASK_AUTHORITY_AUTHORIZED ((ayni_task_authority_phase_t)1)
/* Since 0.24. Explicit authorization resume is required. */
#define AYNI_TASK_AUTHORITY_PAUSED ((ayni_task_authority_phase_t)2)

/* Since 0.24. Caller-owned Task admission and current refusal reason. */
typedef struct ayni_task_authority_info_v1 {
    uint32_t struct_size;              /* sizeof this output. */
    ayni_task_authority_phase_t phase; /* UNKNOWN iff admission is absent. */
    uint64_t task_id;                  /* Exact nonzero Task identity. */
    uint64_t goal_id;               /* Owning Goal whose immutable ceiling this Task inherited. */
    uint64_t binding_commit_seq;    /* Inherited Goal binding sequence; 0 iff absent. */
    uint64_t transition_id;         /* Last admission update; 0 iff absent. */
    uint64_t commit_seq;            /* Same update's log sequence; 0 iff absent. */
    uint32_t requirement_count;     /* Copied references; 0 may be explicit admission. */
    ayni_reason_t effective_reason; /* Current refusal, or NONE when authorized. */
} ayni_task_authority_info_v1;

/* Since 0.24. One copied historical selection, ordered by Goal requirement. */
typedef struct ayni_task_authority_reference_v1 {
    uint32_t struct_size;       /* sizeof this output. */
    uint32_t grant_revision;    /* Nonzero selected revision, never live-state mirror. */
    uint64_t grant_id;          /* Exact nonzero selected Grant identity. */
    uint32_t requirement_index; /* Zero-based index in the owning Goal's immutable ceiling. */
    uint32_t reserved;          /* Always 0. */
} ayni_task_authority_reference_v1;

/* Since 0.24. Owner-thread copying queries, same lifecycle states as get_task.
 * Unknown Task/index is NOT_FOUND; every failure preserves the entire output.
 * Scope/domain remain queryable from the owning Goal, without Task copies. */
ayni_status_t ayni_get_task_authority(const ayni_kernel_t *kernel, uint64_t task_id,
                                      ayni_task_authority_info_v1 *out_info);
ayni_status_t ayni_get_task_authority_reference(const ayni_kernel_t *kernel, uint64_t task_id,
                                                uint32_t index,
                                                ayni_task_authority_reference_v1 *out_info);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_AUTHORITY_H */
