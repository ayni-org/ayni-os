/* Trusted observation sources and current-boot facts, Since 0.35.
 * Registration and synchronous publication are owner-thread Host operations.
 * Since 0.42, trusted Host workers may enqueue copied samples/gaps; the Kernel
 * owner applies them. These interfaces do not start collection or grant sensor access.
 */
#ifndef AYNI_OBSERVATIONS_H
#define AYNI_OBSERVATIONS_H
#include <ayni/ayni.h>
#include <ayni/media.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum startup observation sources. Zero configured capacity disables them. Since 0.35. */
#define AYNI_OBSERVATION_SOURCE_CAPACITY_MAX 64u
/* Maximum observation predicates in a Scene. Since 0.35. */
#define AYNI_OBSERVATION_CONDITION_COUNT_MAX 8u
/* Default queued Host samples when sources are enabled. Since 0.42. */
#define AYNI_OBSERVATION_INGRESS_CAPACITY_DEFAULT 16u
/* Maximum bounded sample queue entries. Since 0.42. */
#define AYNI_OBSERVATION_INGRESS_CAPACITY_MAX 256u

typedef uint32_t ayni_observation_kind_t;
/* Exact 0/1 value with unit "bool". Since 0.35. */
#define AYNI_OBSERVATION_BOOL ((ayni_observation_kind_t)1)
/* Signed integer in an explicit registered unit. Since 0.35. */
#define AYNI_OBSERVATION_I64 ((ayni_observation_kind_t)2)
typedef uint32_t ayni_observation_origin_t;
/* Trusted local measurement source, not model inference. Since 0.35. */
#define AYNI_OBSERVATION_MEASURED ((ayni_observation_origin_t)1)
/* Inference remains inference even if its value matches a measurement. Since 0.35. */
#define AYNI_OBSERVATION_INFERRED ((ayni_observation_origin_t)2)
typedef uint32_t ayni_observation_quality_t;
/* Source reports all declared quality checks passed. Since 0.35. */
#define AYNI_OBSERVATION_GOOD ((ayni_observation_quality_t)1)
/* Retained for diagnostics; cannot satisfy a Scene prerequisite. Since 0.35. */
#define AYNI_OBSERVATION_DEGRADED ((ayni_observation_quality_t)2)

/* Copied trusted source type; kind/origin/unit cannot change while running. Since 0.35. */
typedef struct ayni_observation_source_v1 {
    uint32_t struct_size;             /* Exact sizeof. */
    uint32_t version;                 /* Positive semantic source version. */
    ayni_string_v1 source_id;         /* Nonempty unique catalog ID. */
    ayni_observation_kind_t kind;     /* BOOL or I64. */
    ayni_observation_origin_t origin; /* MEASURED or INFERRED; never promoted by a sample. */
    ayni_string_v1 unit;              /* Exact nonempty unit, "bool" for BOOL. */
    uint64_t validity_max_us;         /* Positive finite local lifetime limit per sample. */
} ayni_observation_source_v1;

/* Exact dependency identity; old boots/generations/sequences never alias. Since 0.35. */
typedef struct ayni_observation_ref_v1 {
    ayni_catalog_name_v1 source_id; /* Copied nonempty catalog identity. */
    uint32_t source_version;        /* Exact registered semantic version, positive. */
    uint64_t boot_epoch;            /* Kernel boot that accepted this observation. */
    uint64_t generation;            /* Positive collection generation owned by producer. */
    uint64_t sequence;              /* Positive source sequence within that generation. */
    uint64_t subscription_id; /* Since 0.43. 0 for trusted Host facts; otherwise exact Task call. */
} ayni_observation_ref_v1;

/* Owner-thread publication; all fields copied, no media or arbitrary paths. Since 0.35. */
typedef struct ayni_observation_v1 {
    uint32_t struct_size;     /* Exact sizeof. */
    uint32_t source_version;  /* Exact registered source version. */
    ayni_string_v1 source_id; /* Registered source; not a routing instruction. */
    uint64_t generation;      /* Positive; increasing generation invalidates old references. */
    uint64_t sequence;        /* Positive and increasing within the current generation. */
    uint64_t captured_at_us;  /* Current Kernel clock domain; no future timestamp. */
    uint64_t valid_for_us;    /* Positive, <= source limit; no overflow/truncation. */
    ayni_observation_quality_t quality; /* GOOD or DEGRADED. */
    uint32_t reserved;                  /* Zero. */
    int64_t value;                      /* BOOL is exactly 0/1; I64 uses the registered unit. */
} ayni_observation_v1;

/* Any non-ISR Host thread. Acceptance copies metadata into a bounded FIFO;
 * it does not claim the owner accepted a fresh observation. Samples do not
 * merge or evict control/input events; a full queue returns BUSY. Since 0.42. */
typedef struct ayni_observation_post_v1 {
    uint32_t struct_size; /* Exact sizeof. */
    uint32_t reserved;    /* Zero. */
    uint64_t boot_epoch;  /* Exact current Kernel boot, captured before launching the producer. */
    ayni_observation_v1 sample; /* Copied intrinsic fields; owner later checks capture/expiry. */
    const ayni_media_ref_v1 *media; /* Optional copied metadata; never retained as a pointer. */
} ayni_observation_post_v1;

/* Explicit telemetry discontinuity; retained separately from the bounded sample
 * FIFO so a full queue cannot forget it. No sample value is fabricated. Since 0.42. */
typedef struct ayni_observation_gap_v1 {
    uint32_t struct_size;     /* Exact sizeof. */
    uint32_t source_version;  /* Exact registered semantic source version. */
    uint64_t boot_epoch;      /* Exact producer boot; old posts return STALE. */
    ayni_string_v1 source_id; /* Copied nonempty registered source. */
    uint64_t generation;      /* Positive; invalidates every older generation. */
    uint64_t
        through_sequence; /* Invalid through this sequence; 0 invalidates older generations only. */
} ayni_observation_gap_v1;

/* Consistent producer/owner diagnostics; all counts are since Kernel creation.
 * Reading these counters changes no lifecycle or delivery state. Since 0.42. */
typedef struct ayni_observation_ingress_info_v1 {
    uint32_t struct_size;   /* Caller supplies exact sizeof. */
    uint32_t capacity;      /* Configured FIFO entries; 0 means disabled. */
    uint32_t queued;        /* Accepted samples not yet consumed, <= capacity. */
    uint32_t gaps_pending;  /* Pending Host/scoped gaps, <= source capacity + call capacity. */
    uint64_t accepted;      /* Samples copied by successful enqueue. */
    uint64_t rejected_busy; /* Samples rejected due to the queue bound. */
    uint64_t gap_updates;   /* Forward gap-frontier updates; repeated markers do not increment. */
    uint64_t discarded;     /* Samples discarded on close or rejected by owner validation. */
} ayni_observation_ingress_info_v1;

/* Finite Scene predicate; only GOOD measurements can satisfy it. Since 0.35. */
typedef struct ayni_observation_condition_v1 {
    ayni_string_v1 source_id; /* Exact registered source. */
    uint32_t source_version;  /* Exact semantic version; positive. */
    uint32_t reserved;        /* Zero. */
    int64_t minimum;          /* Inclusive lower bound in the source's registered unit. */
    int64_t maximum;          /* Inclusive upper bound, >= minimum. */
    uint64_t max_age_us;      /* Positive finite age bound, independent of sample TTL. */
} ayni_observation_condition_v1;

/* Copied diagnostics; expiry never makes stale data usable again. Since 0.35. */
typedef struct ayni_observation_info_v1 {
    uint32_t struct_size; /* Caller supplies exact sizeof. */
    uint32_t usable;      /* 1 iff current, GOOD and unexpired; origin remains explicit. */
    ayni_observation_ref_v1 reference;  /* Complete source identity of the retained sample. */
    ayni_observation_kind_t kind;       /* Registered representation. */
    ayni_observation_origin_t origin;   /* Registered classification. */
    ayni_observation_quality_t quality; /* Latest quality fact. */
    uint32_t reserved;                  /* Written zero. */
    ayni_catalog_name_v1 unit;          /* Copied registered unit. */
    uint64_t captured_at_us;            /* Accepted local capture time. */
    uint64_t valid_until_us;            /* Capture + valid_for, exclusive deadline. */
    int64_t value;                      /* Typed scalar; not an assertion of current validity. */
    /* Since 0.42. Opaque current sample metadata; media_id 0 means no media.
     * A stale/expired sample also makes this reference unusable. */
    ayni_media_ref_v1 media;
    uint64_t sequence_gaps; /* Since 0.42. Explicit or observed skipped source sequences. */
} ayni_observation_info_v1;

/* CONFIGURING, copied descriptor; standard argument/version/capacity errors. Since 0.35. */
ayni_status_t ayni_register_observation_source(ayni_kernel_t *kernel,
                                               const ayni_observation_source_v1 *source);
/* RUNNING trusted Host, no persistence; stale/invalid publication has no effect.
 * OK, INVALID_ARGUMENT/STATE, UNSUPPORTED_VERSION, NOT_READY, NOT_FOUND,
 * STALE or EXPIRED; generation/sequence are not automatically incremented. Since 0.35. */
ayni_status_t ayni_publish_observation(ayni_kernel_t *kernel,
                                       const ayni_observation_v1 *observation);
/* Any non-ISR Host thread, RUNNING only, all producer threads joined before
 * destroy. OK means copied; BUSY means caller retains ownership and may retry.
 * NOT_READY, UNSUPPORTED (queue disabled), STALE (boot), INVALID_ARGUMENT,
 * UNSUPPORTED_VERSION and NOT_FOUND preserve the queue. Since 0.42. */
ayni_status_t ayni_post_observation(ayni_kernel_t *kernel, const ayni_observation_post_v1 *post);
/* Same lifetime/thread rules; a forward marker always has reserved source
 * capacity and wakes the owner. Repeated markers are idempotent. Since 0.42. */
ayni_status_t ayni_report_observation_gap(ayni_kernel_t *kernel,
                                          const ayni_observation_gap_v1 *gap);
/* Any non-ISR thread; immutable lifetime plus ingress lock, no owner-state read.
 * Argument/size failures preserve output. Since 0.42. */
ayni_status_t ayni_get_observation_ingress(const ayni_kernel_t *kernel,
                                           ayni_observation_ingress_info_v1 *out_info);
/* Owner-thread copy. NOT_FOUND if no retained sample, other failures preserve out.
 * INVALID_ARGUMENT/STATE, UNSUPPORTED_VERSION follow ordinary Kernel queries. Since 0.35. */
ayni_status_t ayni_get_observation(const ayni_kernel_t *kernel, ayni_string_v1 source_id,
                                   ayni_observation_info_v1 *out_info);

/* Lock-free latest-wins slot diagnostics; all counts are since Kernel creation. Since 0.59. */
typedef struct ayni_observation_slot_info_v1 {
    uint32_t struct_size;  /* Caller supplies exact sizeof. */
    uint32_t source_count; /* Registered sources with lock-free latest slots; 0 if disabled. */
    uint64_t published;    /* Successful latest-slot writes. */
    uint64_t overwritten;  /* Unread prior samples superseded by newer writes. */
    uint64_t applied;      /* Drained samples accepted into ObservationTable. */
    uint64_t discarded;    /* Drained samples rejected by owner clock/sequence checks. */
} ayni_observation_slot_info_v1;

/* Any non-ISR Host thread, RUNNING only. Lock-free triple-buffered latest-wins overwrite per
 * registered source; never returns BUSY under high sensor rates and wakes the Kernel owner
 * when transitioning a slot from clean to dirty. Since 0.59. */
ayni_status_t ayni_publish_observation_latest(ayni_kernel_t *kernel,
                                              const ayni_observation_post_v1 *post);
/* Any non-ISR thread; lock-free diagnostic snapshot of latest-wins observation slots.
 * Argument/size failures preserve output. Since 0.59. */
ayni_status_t ayni_get_observation_slots(const ayni_kernel_t *kernel,
                                         ayni_observation_slot_info_v1 *out_info);

#ifdef __cplusplus
}
#endif
#endif
