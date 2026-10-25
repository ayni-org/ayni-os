/* Trusted observation sources and current-boot facts, Since 0.35.
 * Registration and publication are owner-thread Host operations, never callable
 * from Provider workers. Subscription ingress will feed the same owner; this
 * initial interface does not start collection or grant access to a sensor.
 */
#ifndef AYNI_OBSERVATIONS_H
#define AYNI_OBSERVATIONS_H
#include <ayni/ayni.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum startup observation sources. Zero configured capacity disables them. Since 0.35. */
#define AYNI_OBSERVATION_SOURCE_CAPACITY_MAX 64u
/* Maximum observation predicates in a Scene. Since 0.35. */
#define AYNI_OBSERVATION_CONDITION_COUNT_MAX 8u

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
} ayni_observation_info_v1;

/* CONFIGURING, copied descriptor; standard argument/version/capacity errors. Since 0.35. */
ayni_status_t ayni_register_observation_source(ayni_kernel_t *kernel,
                                               const ayni_observation_source_v1 *source);
/* RUNNING trusted Host, no persistence; stale/invalid publication has no effect.
 * OK, INVALID_ARGUMENT/STATE, UNSUPPORTED_VERSION, NOT_READY, NOT_FOUND,
 * STALE or EXPIRED; generation/sequence are not automatically incremented. Since 0.35. */
ayni_status_t ayni_publish_observation(ayni_kernel_t *kernel,
                                       const ayni_observation_v1 *observation);
/* Owner-thread copy. NOT_FOUND if no retained sample, other failures preserve out.
 * INVALID_ARGUMENT/STATE, UNSUPPORTED_VERSION follow ordinary Kernel queries. Since 0.35. */
ayni_status_t ayni_get_observation(const ayni_kernel_t *kernel, ayni_string_v1 source_id,
                                   ayni_observation_info_v1 *out_info);

#ifdef __cplusplus
}
#endif
#endif
