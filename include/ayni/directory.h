/* Task-scoped, currently authorized capability metadata, Since 0.49.
 * Configuration adopts candidates explicitly; discovery never creates Grants.
 */
#ifndef AYNI_DIRECTORY_H
#define AYNI_DIRECTORY_H
#include <ayni/tool_usage.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Adopted summary UTF-8 byte bound. Since 0.49. */
#define AYNI_CAPABILITY_SUMMARY_MAX_BYTES 256u
/* Entries per list call, independent of total directory size. Since 0.49. */
#define AYNI_CAPABILITY_PAGE_MAX 16u
/* Fixed ABI value layout; any future fields require a new value type.
 * First page uses an all-zero cursor. A returned cursor is tied to this boot,
 * Task and current work/authority/durability prefix; expiration or a changed
 * prefix returns REVISION_CONFLICT. Cursor bytes confer no authority. */
typedef struct ayni_capability_cursor_v1 {
    uint64_t boot_epoch;     /* Positive Kernel boot identity; zero only for first page. */
    uint64_t task_id;        /* Exact owning Task ID; zero only for first-page cursor. */
    uint64_t commit_seq;     /* Work prefix sequence sampled for this page. */
    uint64_t durable_seq;    /* Acknowledged writer prefix, same sampling boundary. */
    uint64_t valid_until_us; /* Monotonic us; UINT64_MAX when no finite Grant deadline exists. */
    uint32_t gate_count;     /* Accepted pending deny gates; zero means none. */
    uint32_t position;       /* Frozen policy position of the next scan. */
} ayni_capability_cursor_v1;
/* Fixed ABI value layout; extensions require a new value type.
 * Borrowed immutable metadata until Kernel destruction; visibility is checked
 * on every list/inspect/call and is never promised by retaining this value. */
typedef struct ayni_capability_info_v1 {
    ayni_string_v1 binding_id;    /* Exact nonempty frozen Binding identity, borrowed. */
    uint32_t binding_version;     /* Positive exact Binding version, no latest fallback. */
    ayni_string_v1 capability_id; /* Exact nonempty Capability selected by this Binding. */
    uint32_t capability_version;  /* Positive exact Capability version. */
    ayni_tool_role_t role;        /* CAPABILITY or the trusted single SHELL role. */
    ayni_string_v1 summary;       /* Borrowed UTF-8 bytes, 1..SUMMARY_MAX_BYTES. */
} ayni_capability_info_v1;
/* Caller owns fields; successful inspection rebases usage.fields into this
 * structure. Definition bytes borrow immutable catalogs, with no private data. */
typedef struct ayni_capability_detail_v1 {
    uint32_t struct_size;            /* Exact sizeof this ABI minor layout. */
    uint32_t reserved;               /* Must be zero. */
    ayni_capability_info_v1 info;    /* Same exact visible selection as list. */
    ayni_bytes_v1 original_domain;   /* Borrowed canonical authority definition bytes. */
    ayni_bytes_v1 business_contract; /* Borrowed complete machine Schema/projection proof. */
    ayni_tool_usage_v1 usage;        /* Borrowed strings; fields points into this output. */
    ayni_tool_field_usage_v1
        fields[AYNI_AUTHORITY_FIELD_COUNT_MAX]; /* Used prefix: usage.field_count. */
} ayni_capability_detail_v1;
/* CONFIGURING, owner thread, nonreentrant. Copies exact-pair usage plus nonempty
 * UTF-8 summary <=SUMMARY_MAX_BYTES. Register capability and Binding first.
 * Duplicate metadata returns INVALID_ARGUMENT; malformed/mismatched returns
 * INVALID_ARGUMENT. No callback, backend access or implicit trusted adoption. */
ayni_status_t ayni_register_capability_usage(ayni_kernel_t *kernel, ayni_string_v1 summary,
                                             const ayni_tool_usage_v1 *usage);
/* RUNNING owner thread, nonreentrant. capacity=1..PAGE_MAX. Failure preserves
 * items/count/next; zero next denotes no further page (count may be zero). Unknown/unlisted/
 * unauthorized/missing-metadata tools all return NOT_FOUND from inspect.
 * List includes only frozen policy entries with compatible live complete Grant
 * chains. It does not predict whether particular arguments will be accepted. */
ayni_status_t ayni_list_task_capabilities(const ayni_kernel_t *kernel, uint64_t task_id,
                                          const ayni_capability_cursor_v1 *cursor,
                                          ayni_capability_info_v1 *items, uint32_t capacity,
                                          uint32_t *out_count, ayni_capability_cursor_v1 *out_next);
ayni_status_t ayni_inspect_task_capability(const ayni_kernel_t *kernel, uint64_t task_id,
                                           ayni_string_v1 binding_id, uint32_t binding_version,
                                           ayni_capability_detail_v1 *out);
/* Management evidence for one bound optional Call. This query confers no
 * authority and exposes no argument/Scope body. GOAL_REAUTHORIZE operates on
 * the entire direct-live cohort present at application, not a tool-set CAS. */
typedef struct ayni_task_tool_authority_v1 {
    uint32_t struct_size;       /* Exact sizeof this ABI minor layout. */
    uint32_t reserved;          /* Must be zero. */
    uint64_t task_id;           /* Positive owning Task ID, never a slot index. */
    uint64_t source_call_id;    /* Positive original Call identity, retained for this Task. */
    uint64_t original_grant_id; /* Positive immutable whole-Scope ceiling anchor. */
    uint64_t selected_grant_id; /* Positive current selection; changes only by reauthorization. */
    uint32_t selected_revision; /* Positive revision selected with that Grant. */
    uint32_t reserved_tail;     /* Must be zero. */
    uint64_t authority_commit_seq; /* Last Task authorization update, diagnostic rather than CAS. */
} ayni_task_tool_authority_v1;
ayni_status_t ayni_get_task_tool_authority(const ayni_kernel_t *kernel, uint64_t task_id,
                                           uint32_t index, ayni_task_tool_authority_v1 *out);
#ifdef __cplusplus
}
#endif
#endif
