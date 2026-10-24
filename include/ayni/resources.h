/* Trusted resource configuration and diagnostics (ADR-0042), Since 0.29.
 * Owner thread only, never reentrant from a Kernel callback. Registration
 * copies all fields in CONFIGURING; health is volatile and starts OFFLINE.
 * These functions do not grant access or expose resource acquisition to
 * applications. Capability calls acquire resources through Kernel admission.
 */
#ifndef AYNI_RESOURCES_H
#define AYNI_RESOURCES_H

#include <ayni/ayni.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t ayni_resource_kind_t;
/* Exactly one unit and one holder. Since 0.29. */
#define AYNI_RESOURCE_EXCLUSIVE ((ayni_resource_kind_t)1)
/* Independent units, bounded by the registered capacity. Since 0.29. */
#define AYNI_RESOURCE_SHARED ((ayni_resource_kind_t)2)
/* An unconfirmed release requires an exact stop confirmation. Since 0.29. */
#define AYNI_RESOURCE_ACTUATOR 1u

typedef uint32_t ayni_resource_health_t;
/* Not available; this does not discard existing holders. Since 0.29. */
#define AYNI_RESOURCE_OFFLINE ((ayni_resource_health_t)1)
/* Available subject to occupancy and quarantine. Since 0.29. */
#define AYNI_RESOURCE_ONLINE ((ayni_resource_health_t)2)
/* Health check failed; existing holders remain accounted. Since 0.29. */
#define AYNI_RESOURCE_FAULTED ((ayni_resource_health_t)3)

typedef uint32_t ayni_resource_state_t;
/* Online with no holders. Since 0.29. */
#define AYNI_RESOURCE_FREE ((ayni_resource_state_t)1)
/* One or more units held; a shared resource may have spare units. Since 0.29. */
#define AYNI_RESOURCE_HELD ((ayni_resource_state_t)2)
/* An actuator lacks stop confirmation; health cannot clear it. Since 0.29. */
#define AYNI_RESOURCE_QUARANTINED ((ayni_resource_state_t)3)
/* Offline or faulted; consult health for the cause. Since 0.29. */
#define AYNI_RESOURCE_UNAVAILABLE ((ayni_resource_state_t)4)

/* Immutable trusted description. Only ACTUATOR is a supported flag;
 * actuators must be exclusive. Shared capacity is 1..UINT32_MAX units.
 * Registration copies resource_id, a valid catalog ID; no view is retained.
 */
typedef struct ayni_resource_desc_v1 {
    uint32_t struct_size;       /* sizeof(ayni_resource_desc_v1). */
    ayni_resource_kind_t kind;  /* EXCLUSIVE or SHARED; no zero/default. */
    ayni_string_v1 resource_id; /* Unique ID in this Kernel; never empty. */
    uint32_t capacity;          /* Positive units; EXCLUSIVE requires 1. */
    uint32_t flags;             /* 0 or AYNI_RESOURCE_ACTUATOR. */
} ayni_resource_desc_v1;

/* Copied runtime diagnostics. Health has precedence in state; quarantine_id
 * remains visible while offline/faulted. All fields describe this boot only.
 */
typedef struct ayni_resource_info_v1 {
    uint32_t struct_size;           /* Caller supplies sizeof. */
    ayni_resource_state_t state;    /* Derived from health, quarantine and used. */
    ayni_resource_health_t health;  /* Trusted latest report; initially OFFLINE. */
    uint32_t capacity;              /* Immutable registered units. */
    uint32_t used;                  /* Units held; 0 after release, even in quarantine. */
    uint32_t flags;                 /* Immutable descriptor flags. */
    uint64_t quarantine_id;         /* Releasing call ID; 0 iff no unconfirmed stop. */
    uint64_t quarantine_generation; /* Occupancy generation; 0 iff quarantine_id is 0. */
} ayni_resource_info_v1;

/* CONFIGURING only; duplicate ID is INVALID_ARGUMENT, full table is
 * CAPACITY_EXCEEDED. Disabled tables have zero capacity. No persistence/I/O.
 */
ayni_status_t ayni_register_resource(ayni_kernel_t *kernel, const ayni_resource_desc_v1 *resource);
/* CONFIGURING or RUNNING only. Does not release held or quarantined units.
 * A changed report wakes the owner. Unknown IDs return NOT_FOUND.
 */
ayni_status_t ayni_set_resource_health(ayni_kernel_t *kernel, ayni_string_v1 resource_id,
                                       ayni_resource_health_t health);
/* Any lifecycle including CONFIGURING/STOPPED; caller owns output. Errors
 * preserve output. Does not expose internal pointers or acquire a resource.
 */
ayni_status_t ayni_get_resource(const ayni_kernel_t *kernel, ayni_string_v1 resource_id,
                                ayni_resource_info_v1 *out_info);

#ifdef __cplusplus
}
#endif
#endif /* AYNI_RESOURCES_H */
