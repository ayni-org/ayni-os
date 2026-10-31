/* Model-neutral bounded business values and schemas (ADR-0084), Since 0.48.
 * All storage is caller-owned; no allocator, JSON library or recursive pointer
 * graph crosses this interface. Pure operations below perform no I/O. */
#ifndef AYNI_VALUES_H
#define AYNI_VALUES_H
#include <ayni/ayni.h>
#include <ayni/workspace.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AYNI_VALUE_DEPTH_MAX 8u
#define AYNI_SCHEMA_NODE_COUNT_MAX 128u
#define AYNI_VALUE_NODE_COUNT_MAX 256u
#define AYNI_VALUE_LIST_COUNT_MAX 64u
#define AYNI_DECIMAL_SCALE_MAX 18u
/* Per-tree UTF-8 bytes, independent of the caller's smaller configured bound. Since 0.48. */
#define AYNI_VALUE_BYTES_MAX AYNI_CALL_INPUT_BYTES_MAX
/* Static field names and units remain finite even before record admission. Since 0.48. */
#define AYNI_SCHEMA_NAME_BYTES_MAX 64u

typedef uint32_t ayni_value_kind_t;
/* Explicit null, allowed only by a nullable schema. Since 0.48. */
#define AYNI_VALUE_NULL ((ayni_value_kind_t)0)
/* Closed object with stable, nonzero child field IDs. Since 0.48. */
#define AYNI_VALUE_OBJECT ((ayni_value_kind_t)1)
/* Ordered list; repeated elements remain distinct. Since 0.48. */
#define AYNI_VALUE_LIST ((ayni_value_kind_t)2)
/* Boolean, represented by exactly zero or one. Since 0.48. */
#define AYNI_VALUE_BOOL ((ayni_value_kind_t)3)
/* Exact signed integer. Since 0.48. */
#define AYNI_VALUE_I64 ((ayni_value_kind_t)4)
/* Exact unsigned integer. Since 0.48. */
#define AYNI_VALUE_U64 ((ayni_value_kind_t)5)
/* Finite approximate IEEE binary64; never a permission operand. Since 0.48. */
#define AYNI_VALUE_FLOAT64 ((ayni_value_kind_t)6)
/* Exact coefficient times ten to the negative scale. Since 0.48. */
#define AYNI_VALUE_DECIMAL ((ayni_value_kind_t)7)
/* One member of a declared finite UTF-8 set. Since 0.48. */
#define AYNI_VALUE_ENUM ((ayni_value_kind_t)8)
/* A catalog identifier, optionally restricted to a finite set. Since 0.48. */
#define AYNI_VALUE_IDENTIFIER ((ayni_value_kind_t)9)
/* Bounded Unicode scalar UTF-8 text. Since 0.48. */
#define AYNI_VALUE_TEXT ((ayni_value_kind_t)10)

/* Value unit is supplied by its schema; scale is 0..DECIMAL_SCALE_MAX. */
typedef struct ayni_decimal_v1 {
    int64_t coefficient; /* Exact integer coefficient; zero is a value, never absence. */
    uint32_t scale;      /* Decimal places; must equal the schema's declared scale. */
    uint32_t reserved;   /* Must be zero. */
} ayni_decimal_v1;

/* One preorder node. Root/array items have field_id=0; object children have a
 * nonzero stable field_id. subtree_end is an absolute exclusive node index;
 * a scalar ends at its own index+1. Only the kind's union member is active. */
typedef struct ayni_value_node_v1 {
    ayni_value_kind_t kind; /* Explicit kind; never inferred from an empty union. */
    uint32_t field_id;      /* Zero for root/list items, otherwise the object's field identity. */
    uint32_t subtree_end;   /* Exclusive preorder index; root covers the whole node table. */
    uint32_t reserved;      /* Must be zero; ABI padding/inactive union bytes are ignored. */
    union {
        uint32_t boolean;          /* BOOL: exactly 0 or 1. */
        int64_t integer;           /* I64: exact signed value. */
        uint64_t unsigned_integer; /* U64: exact unsigned value. */
        double approximate;        /* FLOAT64: finite; negative zero canonicalizes to positive. */
        ayni_decimal_v1 decimal;   /* DECIMAL: coefficient/scale as declared above. */
        ayni_string_v1 text;       /* TEXT/ENUM/IDENTIFIER: borrowed bytes, including empty TEXT. */
    } value;
} ayni_value_node_v1;

/* One complete immutable tree, borrowed for the duration of a pure operation. */
typedef struct ayni_value_v1 {
    const ayni_value_node_v1 *nodes; /* Nonnull; no node may refer to caller output storage. */
    uint32_t node_count;             /* 1..VALUE_NODE_COUNT_MAX; root covers this entire prefix. */
} ayni_value_v1;

#define AYNI_SCHEMA_REQUIRED 1u
#define AYNI_SCHEMA_NULLABLE 2u

/* A schema is also a preorder tree. An object contains its field schemas;
 * a list contains exactly one item schema. Scalar bounds apply to its kind
 * only; unrelated bound fields are ignored, not read as another union type. */
typedef struct ayni_schema_node_v1 {
    ayni_value_kind_t kind; /* Concrete non-null kind; nullability uses the explicit flag. */
    uint32_t field_id;      /* Zero for root/list item; nonzero for an object field. */
    uint32_t subtree_end;   /* Exclusive preorder index within the schema node table. */
    uint32_t flags;         /* REQUIRED/NULLABLE only; root/list item cannot be optional. */
    ayni_string_v1 name;    /* Object field's nonempty UTF-8 JSON name; empty otherwise. */
    ayni_string_v1 unit;    /* Static unit, or empty when dimensionless. */
    int64_t min_i64;        /* I64/DECIMAL inclusive lower bound; coefficient for DECIMAL. */
    int64_t max_i64;        /* I64/DECIMAL inclusive upper bound; min<=max. */
    uint64_t min_u64;       /* U64 inclusive lower bound. */
    uint64_t max_u64;       /* U64 inclusive upper bound; min<=max. */
    double min_float64;     /* FLOAT64 finite inclusive lower bound. */
    double max_float64;     /* FLOAT64 finite inclusive upper bound; min<=max. */
    uint32_t min_count;     /* LIST elements or TEXT bytes; zero explicitly permits empty. */
    uint32_t max_count;     /* LIST elements or TEXT bytes; min<=max, never an absence flag. */
    uint32_t scale;         /* DECIMAL places, 0..DECIMAL_SCALE_MAX. */
    uint32_t choice_count;  /* ENUM positive; IDENTIFIER zero means any valid catalog ID. */
    const ayni_string_v1 *choices;      /* Static distinct members; null iff choice_count=0. */
    const ayni_value_v1 *default_value; /* Optional field's static default, or null for none. */
} ayni_schema_node_v1;

/* Complete static declaration; registration must copy it and all referenced bytes. */
typedef struct ayni_schema_v1 {
    const ayni_schema_node_v1 *nodes; /* Nonnull, immutable through the operation. */
    uint32_t node_count;              /* 1..SCHEMA_NODE_COUNT_MAX. */
} ayni_schema_v1;

/* Output arena. On OK all text views refer to bytes. On BUFFER_TOO_SMALL only
 * node_count/byte_count change to the complete requirement; no buffer is written.
 * On another error every output remains unchanged. Used prefixes exclude tails. */
typedef struct ayni_value_buffer_v1 {
    ayni_value_node_v1 *nodes; /* Caller-owned aligned storage, null only when node_capacity=0. */
    uint32_t node_capacity;    /* Nodes available; never inferred from byte capacity. */
    uint32_t node_count;       /* Published count, or complete requirement on too-small output. */
    char *bytes;               /* Text storage, null only when byte_capacity=0. */
    uint32_t byte_capacity;    /* Bytes available, excluding node storage. */
    uint32_t byte_count;       /* Published text bytes, or complete requirement. */
} ayni_value_buffer_v1;

/* Pure schema admission, including defaults and exact static bounds. Since 0.48. */
/* Since 0.53: explicit region follows ayni_value_workspace_requirements. */
ayni_status_t ayni_schema_validate(const ayni_workspace_v1 *region, const ayni_schema_v1 *schema);
/* Pure tree/representation admission, independent of a schema. Since 0.48. */
ayni_status_t ayni_value_validate(const ayni_value_v1 *value);
/* Shared semantic validator/default filler; normalizes object order and zero.
 * Inputs/output/scratch must not overlap. Concurrent calls may use independent
 * storage; this does not make Kernel or callback entrypoints reentrant. Since 0.48. */
/* Since 0.53: explicit region follows ayni_value_workspace_requirements. */
ayni_status_t ayni_value_normalize(const ayni_workspace_v1 *region, const ayni_schema_v1 *schema,
                                   const ayni_value_v1 *input, ayni_value_buffer_v1 *output);
/* Portable value encoding, independent of native struct padding. Writes only
 * after full shape/capacity admission; BUFFER_TOO_SMALL sets out_size. Since 0.48. */
ayni_status_t ayni_value_write(const ayni_value_v1 *value, uint8_t *out, uint32_t capacity,
                               uint32_t *out_size);
/* Decode into caller node storage. Text borrows body until it changes. Too-small
 * sets out_count only; other failures preserve every output. Since 0.48. */
/* Since 0.53: explicit region follows ayni_value_workspace_requirements. */
ayni_status_t ayni_value_read(const ayni_workspace_v1 *region, ayni_bytes_v1 body,
                              ayni_value_node_v1 *nodes, uint32_t capacity, uint32_t *out_count);
/* Parses exact no-exponent decimal text without passing through double.
 * Extra fractional zeroes are accepted; no rounding. Failure preserves out. Since 0.48. */
ayni_status_t ayni_decimal_parse(ayni_string_v1 text, uint32_t scale, ayni_decimal_v1 *out);

#ifdef __cplusplus
}
#endif
#endif
