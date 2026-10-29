/* Model-neutral tool usage supplied by an extension, Since 0.47.
 * This is descriptive data, never authority, executable code or a Core registry.
 */
#ifndef AYNI_TOOL_USAGE_H
#define AYNI_TOOL_USAGE_H
#include <ayni/calls.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Total UTF-8 bytes across all identity and usage strings per tool. Since 0.47. */
#define AYNI_TOOL_USAGE_MAX_BYTES 4096u
/* Maximum UTF-8 explanation bytes for one authoritative field. Since 0.47. */
#define AYNI_TOOL_FIELD_USAGE_MAX_BYTES 256u

/* Explains one exact field; type, unit and bounds still come from Core. */
typedef struct ayni_tool_field_usage_v1 {
    uint32_t struct_size;       /* Exact sizeof this minor's layout. */
    uint32_t field_id;          /* Nonzero authoritative ID; unique within this tool. */
    ayni_string_v1 description; /* Nonempty UTF-8, <=FIELD_USAGE_MAX_BYTES; borrowed. */
} ayni_tool_field_usage_v1;

/* Extension-owned immutable description for one exact Binding/Capability pair.
 * Every string is nonempty UTF-8, length-delimited; no credentials/private data.
 * All strings together must fit TOOL_USAGE_MAX_BYTES, with no truncation.
 */
typedef struct ayni_tool_usage_v1 {
    uint32_t struct_size;         /* Exact sizeof this minor's layout. */
    uint32_t binding_version;     /* Exact version, nonzero; no ID-only fallback. */
    ayni_string_v1 binding_id;    /* Exact authoritative Binding ID. */
    uint32_t capability_version;  /* Exact Capability version, nonzero. */
    ayni_string_v1 capability_id; /* Exact authoritative Capability ID. */
    ayni_string_v1 purpose;       /* When/why to use this capability, never authority. */
    ayni_string_v1 text_usage;    /* Input text grammar/meaning, including how to use empty text. */
    ayni_string_v1 result_usage; /* Meaning of results and refusals, including completion limits. */
    ayni_string_v1 examples;     /* Usage examples; explanatory data, never automatically run. */
    uint32_t field_count;        /* Exactly the Core view's count, <=AUTHORITY_FIELD_COUNT_MAX. */
    uint32_t reserved;           /* Must be zero. */
    const ayni_tool_field_usage_v1
        *fields; /* One explanation per exact field_id; null iff count=0. */
} ayni_tool_usage_v1;

/* Read-only composition port, borrowed until the model Provider has quiesced.
 * Called synchronously on the submit owner thread, not by the I/O worker.
 * Lookup must be bounded, nonblocking and nonreentrant, without I/O or Kernel
 * calls. Input borrows submit storage. Caller sets out->struct_size. On OK,
 * returned views remain valid until the next lookup or submit returns, whichever
 * occurs first; the consumer must copy them before either. Failure leaves out
 * unchanged: NOT_FOUND for no exact pair, UNSUPPORTED_VERSION for unknown layout,
 * INVALID_ARGUMENT for malformed arguments, or CAPACITY_EXCEEDED for limits.
 * Runtime owns pairing/lifetime. Missing, mismatched or invalid required usage
 * rejects submission before HTTP; there is no legacy or Binding-ID description.
 */
typedef struct ayni_tool_usage_port_v1 {
    uint32_t struct_size; /* Exact sizeof this minor's layout; zero is an absent port. */
    uint32_t reserved;    /* Must be zero. */
    void *context;        /* Owner-owned lookup context; no global mutable registry. */
    /* Resolves only an exact pair, with the calling/lifetime rules above. */
    ayni_status_t (*lookup)(void *context, const ayni_tool_v1 *tool, ayni_tool_usage_v1 *out);
} ayni_tool_usage_port_v1;

#ifdef __cplusplus
}
#endif
#endif
