/* Business schemas and finite authority projection (ADR-0084), Since 0.48.
 * Capability definitions supply meaning; this interface cannot infer permission
 * from free text, execute expressions or combine independent Grants. */
#ifndef AYNI_CAPABILITY_VALUES_H
#define AYNI_CAPABILITY_VALUES_H
#include <ayni/authority.h>
#include <ayni/values.h>
#include <ayni/workspace.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Zero in a projection path selects the current list element. Since 0.48. */
#define AYNI_PROJECTION_LIST_ITEM 0u
/* Explicitly map boolean false/true to integer 0/1. Since 0.48. */
#define AYNI_PROJECTION_BOOL_I64 1u

/* One fixed path to a required, non-null scalar after default normalization.
 * A path can contain one LIST_ITEM. All projected list paths must use the same
 * list anchor, so one complete requirement/Grant covers every list element. */
typedef struct ayni_authority_projection_v1 {
    uint32_t field_id;   /* Exact nonzero authority-domain field, once per complete projection. */
    uint32_t path_count; /* 1..VALUE_DEPTH_MAX; no arbitrary callback/expression. */
    uint32_t path[AYNI_VALUE_DEPTH_MAX]; /* Object field IDs, with at most one LIST_ITEM. */
    uint32_t flags;      /* Zero or explicit BOOL_I64 mapping, never implicit coercion. */
    uint32_t scale;      /* DECIMAL's exact scale, otherwise zero; never a rounding instruction. */
    ayni_string_v1 unit; /* Exact authority unit for numeric coefficients, empty for tokens. */
} ayni_authority_projection_v1;

/* Immutable business definition. Registration copies the canonical definition;
 * defaults must be static constants, never credentials or private runtime values. */
typedef struct ayni_business_contract_v1 {
    ayni_schema_v1 input;  /* Complete input schema; MODEL may explicitly use a string. */
    ayni_schema_v1 result; /* Complete ordinary-result schema; tool proposals remain distinct. */
    uint32_t projection_count; /* Exactly the authority domain's field count, including zero. */
    uint32_t reserved;         /* Must be zero. */
    const ayni_authority_projection_v1 *projections; /* Null iff projection_count=0; copied. */
} ayni_business_contract_v1;

/* Pure inspection of a complete portable proof and its original domain. Calls
 * receive once on this thread after complete admission; borrowed Schema/default
 * views expire when receive returns. No Kernel lookup or authority is implied.
 * The receiver must not throw; its nonpositive status is returned unchanged.
 * Copied proof/domain bytes may be inspected on an independent worker. Since 0.48. */
/* Since 0.53: explicit region follows ayni_business_workspace_requirements;
 * nested codec calls need a separate region until receive returns. */
ayni_status_t ayni_business_contract_read(
    const ayni_workspace_v1 *region, ayni_bytes_v1 original_domain, ayni_bytes_v1 original_proof,
    void *context,
    ayni_status_t (*receive)(void *context, const ayni_business_contract_v1 *business));
#ifdef __cplusplus
}
#endif
#endif
