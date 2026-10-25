/* Finite Scene constraints and owner-controlled persistent enablement, Since 0.35.
 * Scenes add restrictions to existing Grants; they never create authority.
 */
#ifndef AYNI_SCENES_H
#define AYNI_SCENES_H
#include <ayni/authority.h>
#include <ayni/observations.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Maximum configured and retained Scene states; zero capacity disables. Since 0.35. */
#define AYNI_SCENE_CAPACITY_MAX 32u
/* Maximum exact Capability/version rules per Scene. Since 0.35. */
#define AYNI_SCENE_RULE_COUNT_MAX 8u

/* One allowed capability scope; omitted fields normalize using its trusted domain. Since 0.35. */
typedef struct ayni_scene_rule_v1 {
    ayni_string_v1 capability_id; /* Already registered authority Capability, copied. */
    uint32_t capability_version;  /* Positive exact version. */
    uint32_t reserved;            /* Zero. */
    ayni_scope_v1 allowed;        /* Finite scope, intersection with the actual execution Grant. */
} ayni_scene_rule_v1;

/* Trusted CONFIGURING registration; capabilities/sources precede their Scene. Since 0.35. */
typedef struct ayni_scene_desc_v1 {
    uint32_t struct_size;            /* Exact sizeof. */
    uint32_t version;                /* Positive Scene semantics version. */
    ayni_string_v1 scene_id;         /* Unique nonempty catalog identity. */
    uint32_t rule_count;             /* 1..SCENE_RULE_COUNT_MAX, unique exact capabilities. */
    uint32_t condition_count;        /* 0..OBSERVATION_CONDITION_COUNT_MAX, unique sources. */
    const ayni_scene_rule_v1 *rules; /* Copied scopes, never retained pointers. */
    const ayni_observation_condition_v1
        *conditions; /* Copied predicates; null only at zero count. */
} ayni_scene_desc_v1;

/* Payload for SCENE_SET_ENABLED, owner MANAGEMENT with stable nonzero command_id.
 * expected_revision is zero before the first saved state, otherwise its current
 * revision. Enabling saves this exact definition; disabling cannot broaden it.
 * A successful close blocks new consumption before its durable receipt. Since 0.35. */
typedef struct ayni_scene_set_enabled_v1 {
    uint32_t struct_size;        /* Exact sizeof. */
    uint32_t enabled;            /* Exactly 0 or 1. */
    ayni_string_v1 scene_id;     /* Registered Scene, or retained missing Scene when disabling. */
    uint32_t definition_version; /* Exact current version when enabling; zero when disabling. */
    uint32_t reserved;           /* Zero. */
} ayni_scene_set_enabled_v1;

/* Owner-thread copied configuration/state diagnostics, not an authorization result. Since 0.35. */
typedef struct ayni_scene_info_v1 {
    uint32_t struct_size; /* Caller supplies exact sizeof. */
    uint32_t revision;    /* Zero iff no persistent owner state has been accepted. */
    uint64_t object_id;   /* Stable Kernel object ID; zero before first saved state. */
    uint32_t enabled;     /* Saved owner preference, including incompatible definitions. */
    uint32_t
        consumable; /* Enabled, exact definition and durable; Grants/observations still checked. */
    uint32_t saved_version;      /* Enabled definition's version, zero before first saved state. */
    uint32_t registered_version; /* Current startup version, zero if missing from this profile. */
    uint64_t requires_seq;    /* State's publication barrier; zero for implicit disabled default. */
    uint32_t rule_count;      /* Current declared rules, zero when unregistered. */
    uint32_t condition_count; /* Current declared prerequisites, zero when unregistered. */
} ayni_scene_info_v1;

/* Owner thread CONFIGURING; copies and normalizes all declarations. Standard
 * argument/version/state/capacity errors; NOT_FOUND for unresolved sources/capabilities. Since
 * 0.35. */
ayni_status_t ayni_register_scene(ayni_kernel_t *kernel, const ayni_scene_desc_v1 *scene);
/* Owner-thread copy, NOT_FOUND for neither registered nor retained Scene;
 * ordinary argument/version/reentry failures preserve output. Since 0.35. */
ayni_status_t ayni_get_scene(const ayni_kernel_t *kernel, ayni_string_v1 scene_id,
                             ayni_scene_info_v1 *out_info);

#ifdef __cplusplus
}
#endif
#endif
