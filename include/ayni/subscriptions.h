/* Task-owned finite collection leases and copied observations. Since 0.43. */
#ifndef AYNI_SUBSCRIPTIONS_H
#define AYNI_SUBSCRIPTIONS_H
#include <ayni/observations.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Default collection lease; Kernel requests renewal at half its duration. Since 0.43. */
#define AYNI_SUBSCRIPTION_LEASE_MS_DEFAULT 30000u
/* Borrowed only during registration, copied with the exact call contract. Since 0.43. */
typedef struct ayni_subscription_contract_v1 {
    uint32_t struct_size;     /* Exact sizeof. */
    uint32_t source_version;  /* Positive exact registered observation version. */
    ayni_string_v1 source_id; /* Exact immutable source, never a routing instruction. */
    uint32_t lease_ms;        /* Positive finite duration; 0 selects LEASE_MS_DEFAULT. */
    uint32_t reserved;        /* Zero. */
} ayni_subscription_contract_v1;
/* Stable subscription is its logical call; every opening has a fresh generation. Since 0.43. */
typedef struct ayni_subscription_ref_v1 {
    uint64_t subscription_id; /* Positive logical call ID; one Task owns it. */
    uint64_t boot_epoch;      /* Positive Kernel boot of this opening. */
    uint64_t generation;      /* Positive incarnation, never reused by this subscription. */
} ayni_subscription_ref_v1;
/* Immutable target address; a post must still supply its original complete reference. Since 0.43.
 */
typedef struct ayni_subscription_sink ayni_subscription_sink_t;
/* Copied before post returns; acceptance is transport acceptance only. Since 0.43. */
typedef struct ayni_subscription_observation_v1 {
    uint32_t struct_size;                  /* Exact sizeof. */
    uint32_t reserved;                     /* Zero. */
    ayni_subscription_ref_v1 subscription; /* Exact opening; no inference from a reused target. */
    ayni_observation_v1 sample; /* Exact bound source and generation; positive ordered sequence. */
    const ayni_media_ref_v1 *media; /* Optional copied metadata, owned by the same Agent. */
} ayni_subscription_observation_v1;
/* Any non-ISR Provider thread; all workers joined before Kernel destruction.
 * NOT_READY before ACTIVE/after close, STALE for an old opening, BUSY when full.
 * Does not grant capture, media read, egress or retention permission. Since 0.43. */
ayni_status_t
ayni_subscription_post_observation(ayni_subscription_sink_t *sink,
                                   const ayni_subscription_observation_v1 *observation);
/* Reliable source-reserved discontinuity; same opening and lifetime rules. Since 0.43. */
ayni_status_t ayni_subscription_report_gap(ayni_subscription_sink_t *sink,
                                           ayni_subscription_ref_v1 subscription,
                                           uint64_t through_sequence);
/* Provider receives this copied value during submit; target lives through drain/join.
 * Provider stops collection at valid_until_us even if close/renew never arrives. Since 0.43. */
typedef struct ayni_subscription_open_v1 {
    uint32_t struct_size;                  /* Exact sizeof. */
    uint32_t source_version;               /* Exact bound source semantics. */
    ayni_subscription_ref_v1 subscription; /* Task-owned call and current opening. */
    ayni_catalog_name_v1 source_id;        /* Copied source identity. */
    uint64_t owner_instance_id;     /* Positive Agent owner, required for media references. */
    uint64_t valid_until_us;        /* Exclusive finite Kernel-monotonic collection deadline. */
    ayni_subscription_sink_t *sink; /* Borrowed immutable post target through drain/join. */
} ayni_subscription_open_v1;
/* Renew request keeps the same opening; ACK echoes sequence on its original completion.
 * It extends collection only after this trusted request, never after a result alone. Since 0.43. */
typedef struct ayni_subscription_renew_v1 {
    uint32_t struct_size;                  /* Exact sizeof. */
    uint32_t reserved;                     /* Zero. */
    ayni_subscription_ref_v1 subscription; /* Exact opening; no automatic generation change. */
    uint64_t sequence;       /* Strictly next lease control sequence; start ACK uses 1. */
    uint64_t valid_until_us; /* Exclusive new finite Kernel-monotonic deadline. */
} ayni_subscription_renew_v1;
typedef uint32_t ayni_subscription_phase_t;
/* Opening accepted, awaiting Provider start confirmation. Since 0.43. */
#define AYNI_SUBSCRIPTION_STARTING ((ayni_subscription_phase_t)1)
/* Provider confirmed this generation, lease and Task authority remain current. Since 0.43. */
#define AYNI_SUBSCRIPTION_ACTIVE ((ayni_subscription_phase_t)2)
/* New samples refused; close has not yet returned the delivery borrow. Since 0.43. */
#define AYNI_SUBSCRIPTION_STOPPING ((ayni_subscription_phase_t)3)
/* Provider confirmed the generation stopped. Since 0.43. */
#define AYNI_SUBSCRIPTION_STOPPED ((ayni_subscription_phase_t)4)
/* Finite collection lease expired; delivery may still await drain. Since 0.43. */
#define AYNI_SUBSCRIPTION_EXPIRED ((ayni_subscription_phase_t)5)
/* Opening/collection failed; inspect status/reason and call effect separately. Since 0.43. */
#define AYNI_SUBSCRIPTION_FAILED ((ayni_subscription_phase_t)6)
/* Copied trusted owner query; no raw byte pointer or private body. Since 0.43. */
typedef struct ayni_subscription_info_v1 {
    uint32_t struct_size;                  /* Caller supplies exact sizeof. */
    ayni_subscription_phase_t phase;       /* One explicit public phase. */
    ayni_subscription_ref_v1 subscription; /* Latest opening, even if stopped. */
    uint64_t task_id;                      /* Immutable owning Task; positive. */
    uint64_t owner_instance_id;            /* Immutable Agent owner; positive. */
    uint64_t lease_sequence; /* Highest confirmed start/renew; 0 before first ACK this opening. */
    uint64_t valid_until_us; /* Current boot lease deadline; 0 without a live lease. */
    ayni_status_t status;    /* Classified call diagnostic, never Provider raw text. */
    ayni_reason_t reason;    /* Classified cause; NONE if no finer diagnostic. */
} ayni_subscription_info_v1;
/* Owner-thread trusted query; ordinary argument/version/owner gate rules. Since 0.43. */
ayni_status_t ayni_get_subscription(const ayni_kernel_t *kernel, uint64_t subscription_id,
                                    ayni_subscription_info_v1 *out_info);
/* Owner-thread copy of this subscription's latest sample; NOT_FOUND until one
 * was accepted. Stopped/old-generation samples remain diagnostic, usable=0. Since 0.43. */
ayni_status_t ayni_get_subscription_observation(const ayni_kernel_t *kernel,
                                                uint64_t subscription_id,
                                                ayni_observation_info_v1 *out_info);
#ifdef __cplusplus
}
#endif
#endif
