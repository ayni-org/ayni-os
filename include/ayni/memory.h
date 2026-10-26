/* Bounded private memory and explicitly saved owner profile. Since 0.40.
 * Owner-thread APIs never wait for storage. Writes use ayni_submit_intent and
 * its durable receipt; queries copy into caller-owned buffers. No model credentials.
 */
#ifndef AYNI_MEMORY_H
#define AYNI_MEMORY_H
#include <ayni/authority.h>
#include <ayni/ayni.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ayni_memory_namespace_t;
/* One Agent instance's private namespace. Since 0.40. */
#define AYNI_MEMORY_PRIVATE ((ayni_memory_namespace_t)1)
/* Bound owner's basic profile, with separate per-Agent access Grants. Since 0.40. */
#define AYNI_MEMORY_PROFILE ((ayni_memory_namespace_t)2)
typedef uint32_t ayni_memory_source_t;
/* Explicit independent owner statement. Since 0.40. */
#define AYNI_MEMORY_USER ((ayni_memory_source_t)1)
/* Model inference; confirmation never changes its source. Since 0.40. */
#define AYNI_MEMORY_MODEL ((ayni_memory_source_t)2)
/* Organized or summarized source material, with retained lineage. Since 0.40. */
#define AYNI_MEMORY_DERIVED ((ayni_memory_source_t)3)
typedef uint32_t ayni_profile_field_t;
/* Private records have no public-profile field. Since 0.40. */
#define AYNI_PROFILE_NONE ((ayni_profile_field_t)0)
/* Explicit preferred name or form of address. Since 0.40. */
#define AYNI_PROFILE_NAME ((ayni_profile_field_t)1)
/* Explicit preferred language. Since 0.40. */
#define AYNI_PROFILE_LANGUAGE ((ayni_profile_field_t)2)
/* Explicit interaction preference. Since 0.40. */
#define AYNI_PROFILE_INTERACTION ((ayni_profile_field_t)3)
typedef uint32_t ayni_memory_state_t;
/* Retained content; lineage still decides current usability. Since 0.40. */
#define AYNI_MEMORY_LIVE ((ayni_memory_state_t)1)
/* Deleted, immediately unavailable; bounded cleanup pending. Since 0.40. */
#define AYNI_MEMORY_TOMBSTONE ((ayni_memory_state_t)2)
/* In-memory content erased; storage cleanup may still be pending. Since 0.40. */
#define AYNI_MEMORY_SCRUBBED ((ayni_memory_state_t)3)
/* Bounded search labels in UTF-8 bytes. Since 0.40. */
#define AYNI_MEMORY_LABEL_BYTES_MAX 64u
#define AYNI_MEMORY_TAGS_MAX 4u
#define AYNI_MEMORY_KEYWORDS_MAX 8u

/* MEMORY_SAVE body; explicit save is confirmed, independent user content.
 * It cannot claim model source, lineage or another actor's authority. */
typedef struct ayni_memory_save_v1 {
    uint32_t struct_size;               /* sizeof this struct; Since 0.40. */
    ayni_memory_namespace_t name_space; /* PRIVATE or PROFILE. */
    uint64_t memory_id;    /* 0 creates with envelope revision 0; otherwise exact CAS. */
    uint64_t namespace_id; /* Agent instance for PRIVATE, bound owner principal for PROFILE. */
    ayni_profile_field_t profile_field; /* NONE for PRIVATE, one M1 basic field for PROFILE. */
    uint32_t reserved;                  /* Must be zero. */
    ayni_string_v1 body;        /* Nonempty UTF-8, copied before return; configured byte limit. */
    const ayni_string_v1 *tags; /* tag_count unique nonempty UTF-8 labels, borrowed. */
    uint32_t tag_count;         /* 0..AYNI_MEMORY_TAGS_MAX; pointer may be null only for 0. */
    uint32_t keyword_count;     /* 0..AYNI_MEMORY_KEYWORDS_MAX. */
    const ayni_string_v1 *keywords; /* keyword_count labels, same ownership as tags. */
} ayni_memory_save_v1;

/* MEMORY_CONFIRM or MEMORY_DELETE body; exact revision is in the intent envelope. */
typedef struct ayni_memory_target_v1 {
    uint32_t struct_size; /* sizeof this struct; Since 0.40. */
    uint32_t reserved;    /* Must be zero. */
    uint64_t memory_id;   /* Nonzero retained live memory. */
} ayni_memory_target_v1;

/* Copied metadata, separate from body-buffer lifetime. Since 0.40. */
typedef struct ayni_memory_info_v1 {
    uint32_t struct_size;               /* Caller initializes to sizeof this struct. */
    uint32_t revision;                  /* Positive exact content/confirmation/deletion revision. */
    uint64_t memory_id;                 /* Stable Kernel identity. */
    uint64_t namespace_id;              /* Agent instance or owner principal. */
    ayni_memory_namespace_t name_space; /* Namespace kind. */
    ayni_profile_field_t profile_field; /* M1 basic profile key or NONE. */
    ayni_memory_source_t source;        /* Original source, unchanged by confirmation. */
    ayni_memory_state_t state;          /* Live, tombstone or scrubbed. */
    uint32_t confirmed;                 /* 0 or 1; model results start unconfirmed. */
    uint32_t usable;     /* 1 only while row and every captured dependency are current/durable. */
    uint32_t body_bytes; /* Retained UTF-8 bytes, zero after scrubbing. */
    uint32_t dependency_count; /* Direct captured sources, including context anchors. */
    uint64_t source_call_id;   /* Originating model call or 0 if none. */
    uint64_t commit_seq;       /* Last mutation's durable-acceptance fence. */
} ayni_memory_info_v1;

/* Bounded exact search; empty keyword/tag means no respective filter. Since 0.40. */
typedef struct ayni_memory_search_v1 {
    uint32_t struct_size;               /* sizeof this struct. */
    ayni_memory_namespace_t name_space; /* PRIVATE or PROFILE. */
    uint64_t namespace_id;  /* Exact namespace owner, never inferred from caller text. */
    uint64_t after_id;      /* Exclusive stable-ID cursor; 0 begins. */
    ayni_string_v1 keyword; /* UTF-8 body substring or exact keyword, <= body maximum. */
    ayni_string_v1 tag;     /* Exact UTF-8 tag, <= LABEL_BYTES_MAX. */
} ayni_memory_search_v1;

/* Fixed M1 purpose codes; free-text purpose claims are not authorization. Since 0.40. */
typedef uint32_t ayni_memory_purpose_t;
#define AYNI_MEMORY_CONTEXT ((ayni_memory_purpose_t)1)  /* Foreground context use. Since 0.40. */
#define AYNI_MEMORY_ORGANIZE ((ayni_memory_purpose_t)2) /* Bounded organization. Since 0.40. */

/* A staged policy write to this Task owner's private namespace. Since 0.40.
 * Initial state is unconfirmed; policy cannot create owner-profile or independent USER facts.
 * Exactly one proposal and one matching WRITE_MEMORY intent are accepted per decision.
 */
typedef struct ayni_memory_proposal_v1 {
    uint32_t struct_size;        /* sizeof this struct. */
    uint32_t expected_revision;  /* 0 for create, otherwise exact target CAS. */
    uint64_t memory_id;          /* 0 creates; nonzero must belong to this Agent. */
    ayni_memory_source_t source; /* MODEL or DERIVED, never USER. */
    uint32_t reserved;           /* Must be zero. */
    uint64_t source_call_id;     /* MODEL: this Task's durable completed model call; DERIVED: 0. */
    ayni_string_v1 step_key;     /* New machine step key, matches the WRITE_MEMORY intent. */
    ayni_string_v1 body;         /* Nonempty UTF-8, copied by propose, within configured limit. */
    const ayni_string_v1 *tags;  /* Borrowed tag_count labels, null only for zero. */
    uint32_t tag_count;          /* At most TAGS_MAX. */
    uint32_t keyword_count;      /* At most KEYWORDS_MAX. */
    const ayni_string_v1 *keywords; /* Borrowed keyword_count labels. */
} ayni_memory_proposal_v1;

/* Borrowed capability of the current decide callback only. Since 0.40.
 * Actor is the Task owner, never an input field. Reads capture the entire current
 * source closure before exposing bytes. Overflow poisons the decision even if
 * strategy code ignores the error. No pointer may be kept beyond decide.
 */
typedef struct ayni_memory_view_v1 {
    void *context; /* Kernel-owned decision lease, never an external Session. */
    ayni_status_t (*read)(void *context, uint64_t memory_id, ayni_memory_purpose_t purpose,
                          ayni_memory_info_v1 *out, char *body, uint32_t capacity,
                          uint32_t *out_bytes); /* Caller-owned copied text, no terminator. */
    ayni_status_t (*search)(void *context, const ayni_memory_search_v1 *query,
                            ayni_memory_purpose_t purpose, ayni_memory_info_v1 *out,
                            uint32_t capacity, uint32_t *out_count);
    /* Same stable-ID pagination; every authorized examined body enters lineage, including
     * nonmatches. Overflow publishes no result and poisons the whole decision. */
    ayni_status_t (*propose)(void *context, const ayni_memory_proposal_v1 *proposal);
    /* Copies one candidate while its borrowed bytes are live; no write until intent acceptance. */
} ayni_memory_view_v1;

/* Bound owner's durable MANAGEMENT session required even for reads. The list contains
 * only live, current records; no unauthorized or deleted body affects matching. Output
 * ascending by ID; count <= capacity, next page uses final returned ID. Empty is OK.
 * Buffers are unchanged on failure. Same owner thread and reentrancy rules as intents.
 */
ayni_status_t ayni_search_memory(const ayni_kernel_t *kernel, uint64_t session_id,
                                 const ayni_memory_search_v1 *query, ayni_memory_info_v1 *out,
                                 uint32_t capacity, uint32_t *out_count);
/* Metadata remains queryable for retained deleted records. With body != NULL, requires
 * current content and enough capacity; BUFFER_TOO_SMALL publishes required byte count
 * only. No terminator is appended. Metadata-only uses body=NULL, capacity=0.
 */
ayni_status_t ayni_get_memory(const ayni_kernel_t *kernel, uint64_t session_id, uint64_t memory_id,
                              ayni_memory_info_v1 *out_info, char *body, uint32_t capacity,
                              uint32_t *out_bytes);
/* Exact memory Grant scope builders; pure shape conversion, no authorization or I/O.
 * Provide four output fields. Unit strings live with Core; export tokens borrow destination
 * until Grant submission returns. Failure preserves fields. Since 0.40. */
#define AYNI_MEMORY_SCOPE_FIELD_COUNT 4u
ayni_status_t ayni_memory_read_scope(uint64_t memory_id, ayni_memory_purpose_t purpose,
                                     ayni_scope_field_v1 *fields, uint32_t capacity);
ayni_status_t ayni_memory_export_scope(uint64_t memory_id, ayni_memory_purpose_t purpose,
                                       const ayni_string_v1 *destination,
                                       ayni_scope_field_v1 *fields, uint32_t capacity);

/* Managed erasure progress, independent of first command acceptance. Since 0.40. */
typedef uint32_t ayni_privacy_phase_t;
#define AYNI_PRIVACY_IDLE ((ayni_privacy_phase_t)0)    /* No invalidation requested. Since 0.40. */
#define AYNI_PRIVACY_ERASING ((ayni_privacy_phase_t)1) /* Bounded owner sweep. Since 0.40. */
#define AYNI_PRIVACY_FILES                                                                         \
    ((ayni_privacy_phase_t)2) /* Waiting for files/transports. Since 0.40.                         \
                               */
#define AYNI_PRIVACY_COMPACTING                                                                    \
    ((ayni_privacy_phase_t)3) /* Replacing both slots/log. Since 0.40. */
#define AYNI_PRIVACY_COMPLETE                                                                      \
    ((ayni_privacy_phase_t)4) /* Complete fact is durable. Since 0.40.                             \
                               */
typedef struct ayni_privacy_info_v1 {
    uint32_t struct_size;
    ayni_privacy_phase_t phase;
    uint64_t request_seq;          /* Latest source invalidation; zero iff IDLE. */
    uint64_t commit_seq;           /* Latest sweep fact; completion waits for durability. */
    ayni_status_t progress_status; /* OK or explicit generation-capacity failure. */
    uint32_t reserved;             /* Always zero. */
} ayni_privacy_info_v1;
/* MANAGEMENT-only owner-thread metadata query. Failure preserves output. */
ayni_status_t ayni_get_privacy_status(const ayni_kernel_t *kernel, uint64_t session_id,
                                      ayni_privacy_info_v1 *out);

#ifdef __cplusplus
}
#endif
#endif
