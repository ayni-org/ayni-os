/* Owner-managed content anchors and file acknowledgement, Since 0.40 (ADR-0060).
 * One anchor per Agent; text remains in the outer store. All calls are owner-thread,
 * non-reentrant and require a durable bound MANAGEMENT session. */
#ifndef AYNI_CONTEXT_H
#define AYNI_CONTEXT_H
#include <ayni/ayni.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ayni_context_action_t;
#define AYNI_CONTEXT_IDLE ((ayni_context_action_t)0)    /* No pending file action. Since 0.40. */
#define AYNI_CONTEXT_PUBLISH ((ayni_context_action_t)1) /* Publish completed Task. Since 0.40. */
#define AYNI_CONTEXT_ERASE_ALL                                                                     \
    ((ayni_context_action_t)2) /* Remove all retained turns. Since 0.40. */
#define AYNI_CONTEXT_ERASE_ROUND                                                                   \
    ((ayni_context_action_t)3) /* Remove one turn and derived text. Since 0.40. */
#define AYNI_CONTEXT_ERASE_DERIVED                                                                 \
    ((ayni_context_action_t)4) /* Source invalidation; preserve user inputs. Since 0.40. */

/* Body of CONTEXT_OPEN/PUBLISH/FORGET/ACK; unused fields must be zero.
 * OPEN: owner_instance_id only, expected_revision zero.
 * PUBLISH: context_id, expected_generation, completed_task_id, next turn_id.
 * FORGET: context_id, expected_generation, ERASE_ALL, ERASE_DERIVED, or ERASE_ROUND and its
 * turn_id. ACK: context_id and expected_generation after matching file fsync/rename/directory
 * fsync. Other operations require the current privacy revision in the intent envelope. ACK never
 * accepts a previous revision/generation. All operations require command_id. */
typedef struct ayni_context_command_v1 {
    uint32_t struct_size;         /* Exact sizeof this version. */
    ayni_context_action_t action; /* FORGET mode, otherwise IDLE. */
    uint64_t owner_instance_id;   /* OPEN target; zero in other commands. Info: owning Agent ID. */
    uint64_t context_id;          /* Stable Kernel ID; zero only in OPEN input. */
    uint64_t expected_generation; /* Exact file version; zero is the initial version. */
    uint64_t completed_task_id;   /* Original completed Task for PUBLISH; zero otherwise. */
    uint64_t turn_id;             /* PUBLISH next turn or ERASE_ROUND target; zero otherwise. */
} ayni_context_command_v1;

typedef struct ayni_context_info_v1 {
    uint32_t struct_size;       /* Exact sizeof this version. */
    uint32_t revision;          /* Positive privacy version; only invalidation advances it. */
    uint64_t context_id;        /* Stable Kernel ID; zero only in OPEN input. */
    uint64_t owner_instance_id; /* OPEN target; zero in other commands. Info: owning Agent ID. */
    uint64_t generation;        /* File generation; zero before first publication/invalidation. */
    uint64_t last_turn_id;      /* Monotone high-water mark; never reset by deletion. */
    ayni_context_action_t pending_action; /* One file action; IDLE has no pending values. */
    uint32_t previous_revision; /* Deletion before-version, zero without pending deletion. */
    uint64_t pending_turn_id;   /* Published/deleted turn number; zero for other actions. */
    uint64_t pending_task_id;   /* PUBLISH source Task; zero for other actions. */
    uint64_t commit_seq;        /* File publication waits until this exact row is durable. */
    uint32_t usable;            /* 1 iff sources and this row are current/durable at query time. */
    uint32_t reserved;          /* Always zero. */
} ayni_context_info_v1;

/* Find the sole anchor for this Agent. NOT_FOUND means it has not been explicitly opened.
 * Failure preserves output; this query exposes metadata, never private file text. */
ayni_status_t ayni_get_context(const ayni_kernel_t *kernel, uint64_t session_id,
                               uint64_t owner_instance_id, ayni_context_info_v1 *out);
#ifdef __cplusplus
}
#endif
#endif
