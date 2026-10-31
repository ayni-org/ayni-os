/* Lossless named arguments from structured adapters, Since 0.48.
 * This is an in-process value view, not a JSON parser or an execution API.
 * The owner resolves names and numeric lexemes with its original Schema. */
#ifndef AYNI_ARGUMENTS_H
#define AYNI_ARGUMENTS_H
#include <ayni/values.h>
#include <ayni/workspace.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Inclusive safe integer magnitude in number-based adapters. Schemas extending
 * this range use STRING for every integer value (ADR-0084). Since 0.48. */
#define AYNI_ARGUMENT_SAFE_INTEGER_MAX INT64_C(9007199254740991)

typedef uint32_t ayni_argument_kind_t;
/* Explicit null; the eventual Schema must allow it. Since 0.48. */
#define AYNI_ARGUMENT_NULL ((ayni_argument_kind_t)0)
/* Closed named object after Schema resolution. Since 0.48. */
#define AYNI_ARGUMENT_OBJECT ((ayni_argument_kind_t)1)
/* Ordered list, including repeated items. Since 0.48. */
#define AYNI_ARGUMENT_LIST ((ayni_argument_kind_t)2)
/* Boolean represented by zero or one. Since 0.48. */
#define AYNI_ARGUMENT_BOOL ((ayni_argument_kind_t)3)
/* Original numeric lexeme; never an intermediate double. Since 0.48. */
#define AYNI_ARGUMENT_NUMBER ((ayni_argument_kind_t)4)
/* Unicode text; includes exact decimal and wide-integer strings. Since 0.48. */
#define AYNI_ARGUMENT_STRING ((ayni_argument_kind_t)5)

/* Preorder source node. Every object child has a nonempty name; root and list
 * items have an empty name. subtree_end follows the value-tree convention. */
typedef struct ayni_argument_node_v1 {
    ayni_argument_kind_t kind; /* Explicit source representation; no Schema kind inference here. */
    uint32_t subtree_end;      /* Exclusive node index; root covers the complete table. */
    uint32_t reserved;         /* Must be zero. */
    ayni_string_v1 name;       /* Nonempty object key, <=SCHEMA_NAME_BYTES_MAX; empty otherwise. */
    union {
        uint32_t boolean;    /* BOOL: exactly zero or one. */
        ayni_string_v1 text; /* STRING or NUMBER: lossless borrowed text. */
    } value;
} ayni_argument_node_v1;

/* Complete immutable source tree; all strings survive through the operation. */
typedef struct ayni_arguments_v1 {
    const ayni_argument_node_v1 *nodes; /* Nonnull for a supplied tree. */
    uint32_t node_count; /* 1..VALUE_NODE_COUNT_MAX; never an implicit empty object. */
} ayni_arguments_v1;

/* Pure shape/name/lexeme admission; no Schema, authorization or callbacks.
 * Unknown/duplicate fields are rejected when resolved against a Schema. Since 0.48. */
ayni_status_t ayni_arguments_validate(const ayni_arguments_v1 *input);
/* Uses the same defaults and canonicalization as native values. Wide integer
 * Schema ranges and decimal values require STRING representation; approximate
 * FLOAT64 requires NUMBER. Schema/input nodes and all their referenced text
 * must not overlap output nodes/bytes; sources remain immutable through return.
 * Independent calls may use independent storage concurrently. Refusals preserve
 * output as ayni_value_normalize does. Since 0.48. */
/* Since 0.53: explicit region follows ayni_value_workspace_requirements. */
ayni_status_t ayni_arguments_normalize(const ayni_workspace_v1 *region,
                                       const ayni_schema_v1 *schema, const ayni_arguments_v1 *input,
                                       ayni_value_buffer_v1 *output);
#ifdef __cplusplus
}
#endif
#endif
