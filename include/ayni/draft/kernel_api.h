/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols; calling these
 * functions fails at link time instead of returning a fabricated result.
 *
 * Planned kernel API beyond ayni.h: catalog registration, further intents and
 * events (plan section 3.2; contracts/domain/lifecycles.md). Implemented
 * functions and types stay in ayni.h and are not redeclared.
 *
 * Conventions for every draft struct and enumeration, as in ayni.h:
 * enumerations are uint32_t typedefs whose states and kinds start at 1, and 0
 * means "none" only where a field may be empty; strings are ayni_string_v1
 * views that the kernel copies during the call and checks against the
 * configured limit (contracts/domain/limits.md); 64-bit identifiers are plain
 * integers here, and only their JSON form is a decimal string
 * (contracts/wire/json_subset.md C-JSON-004).
 *
 * Calling rules (contracts/host/host_ports.md, lifecycles.md):
 * - Every function here runs on the kernel owner thread.
 * - ayni_register_* only in CONFIGURING; descriptors are copied and checked
 *   against the configured limits before return; registration order does not
 *   matter because ayni_start validates all cross references at once.
 * - Return values per lifecycle state follow the table in lifecycles.md.
 */
#ifndef AYNI_DRAFT_KERNEL_API_H
#define AYNI_DRAFT_KERNEL_API_H

#include <ayni/authority.h>
#include <ayni/ayni.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Catalog descriptors (contracts/domain/primitives.md) ------------- */

typedef struct ayni_device_desc_v1 {
    uint32_t struct_size;
    uint32_t reserved; /* 0 */
    ayni_string_v1 device_id;
    ayni_string_v1 host_profile; /* "CORE_LITE", "CORE_FULL" */
    ayni_string_v1 firmware_version;
} ayni_device_desc_v1;

typedef uint32_t ayni_sharing_t;

#define AYNI_SHARING_SHARED ((ayni_sharing_t)1)
#define AYNI_SHARING_EXCLUSIVE ((ayni_sharing_t)2)

/* Resource flags. A control domain carries an authority_epoch (C-RES-012). */
#define AYNI_RESOURCE_CONTROL_DOMAIN 0x1u

typedef struct ayni_resource_desc_v1 {
    uint32_t struct_size;
    ayni_sharing_t sharing;
    ayni_string_v1 resource_id;
    ayni_string_v1 device_id;
    ayni_string_v1 conflict_domain; /* empty: none */
    uint32_t capacity;              /* concurrent holders; 1 for exclusive */
    uint32_t flags;                 /* AYNI_RESOURCE_* */
} ayni_resource_desc_v1;

typedef uint32_t ayni_call_form_t;

#define AYNI_CALL_UNARY ((ayni_call_form_t)1)
#define AYNI_CALL_ACTION ((ayni_call_form_t)2)
#define AYNI_CALL_SUBSCRIPTION ((ayni_call_form_t)3)

/* Side-effect classes, a bit set; 0 means NONE (contracts/domain/
 * calls_and_bindings.md). Each property below is declared independently. */
#define AYNI_EFFECT_DATA_EGRESS 0x1u
#define AYNI_EFFECT_BILLING 0x2u
#define AYNI_EFFECT_NOTIFICATION 0x4u
#define AYNI_EFFECT_PHYSICAL 0x8u

typedef uint32_t ayni_idempotency_t;

#define AYNI_IDEMPOTENT ((ayni_idempotency_t)1)
#define AYNI_NOT_IDEMPOTENT ((ayni_idempotency_t)2)

typedef uint32_t ayni_query_authority_t;

#define AYNI_QUERY_NONE ((ayni_query_authority_t)1)
#define AYNI_QUERY_BEST_EFFORT ((ayni_query_authority_t)2)
#define AYNI_QUERY_AUTHORITATIVE ((ayni_query_authority_t)3)

typedef uint32_t ayni_cancel_mode_t;

#define AYNI_CANCEL_NONE ((ayni_cancel_mode_t)1)
#define AYNI_CANCEL_REQUEST ((ayni_cancel_mode_t)2)
#define AYNI_CANCEL_CONFIRMED ((ayni_cancel_mode_t)3)

/* Future call properties extend the same capability registry (MA-008).
 * No registration symbol accepts this draft; authorization schema
 * registration is published in ayni/authority.h since 0.17. */
typedef struct ayni_capability_call_properties_draft_v1 {
    uint32_t struct_size;
    ayni_call_form_t call_form;
    ayni_string_v1 capability_id; /* e.g. "notify.present" */
    uint32_t version;
    uint32_t effect_classes; /* AYNI_EFFECT_* */
    ayni_idempotency_t idempotency;
    ayni_query_authority_t query_authority;
    ayni_cancel_mode_t cancel_mode;
    uint32_t dedupe_window_ms; /* server-side window keyed by operation_id; 0: none */
    uint32_t deadline_ms;      /* presentation or execution deadline */
    uint32_t required_resource_count;
    const ayni_string_v1 *required_resources; /* resource_id each */
    ayni_string_v1 input_schema;              /* JSON Schema subset, C-JSON-012 */
    ayni_string_v1 result_schema;
    ayni_string_v1 compensation_capability_id; /* empty: none */
} ayni_capability_call_properties_draft_v1;

typedef struct ayni_binding_v1 {
    uint32_t struct_size;
    uint32_t capability_version;
    ayni_string_v1 binding_id;
    ayni_string_v1 capability_id;
    ayni_string_v1 provider_id;
    ayni_string_v1 egress_destination; /* empty: no egress */
    ayni_string_v1 cost_class;         /* empty: free */
    uint32_t resource_count;
    uint32_t reserved; /* 0 */
    const ayni_string_v1 *resources;
} ayni_binding_v1;

ayni_status_t ayni_register_device(ayni_kernel_t *kernel, const ayni_device_desc_v1 *device);
ayni_status_t ayni_register_resource(ayni_kernel_t *kernel, const ayni_resource_desc_v1 *resource);
ayni_status_t ayni_register_binding(ayni_kernel_t *kernel, const ayni_binding_v1 *binding);

/*
 * ayni_start (ayni.h) will also freeze the catalog and validate every cross
 * reference once registration exists: on failure the kernel stays in
 * CONFIGURING and an ayni_error_v1 names the first offending object, so the
 * caller can fix the catalog and call again (C-LIFE-003).
 */

/* ---- Intents --------------------------------------------------------- */

/*
 * Further intent kinds for ayni_intent_v1 and ayni_submit_intent (ayni.h).
 * Until a kind is implemented, ayni_submit_intent returns AYNI_UNSUPPORTED
 * for it. Their body types are fixed together with each kind (C-WIRE-014).
 */
#define AYNI_INTENT_GRANT_CREATE ((ayni_intent_kind_t)9)
#define AYNI_INTENT_GRANT_REVOKE ((ayni_intent_kind_t)10)
#define AYNI_INTENT_MEMORY_WRITE ((ayni_intent_kind_t)11)
#define AYNI_INTENT_MEMORY_DELETE ((ayni_intent_kind_t)12)
#define AYNI_INTENT_SUBSCRIPTION_OPEN ((ayni_intent_kind_t)13)
#define AYNI_INTENT_SUBSCRIPTION_CLOSE ((ayni_intent_kind_t)14)
/* Whether the kernel or the assistant coordinator owns the pause set is
 * decided with MA-019 (ADR-0018). */
#define AYNI_INTENT_ASSISTANT_PAUSE ((ayni_intent_kind_t)15)
#define AYNI_INTENT_ASSISTANT_RESUME ((ayni_intent_kind_t)16)

/* ---- Events ---------------------------------------------------------- */

/*
 * Further event kinds for ayni_event_v1 and ayni_post_event (ayni.h). Until a
 * kind is implemented, ayni_post_event returns AYNI_UNSUPPORTED for it.
 * Observations and telemetry come with subscriptions (MA-018), which append
 * the fields they need to ayni_event_v1: subscription_id, generation,
 * sequence, captured_at_us (host monotonic time of capture), coalesce_key
 * and a body of at most event_body_max_bytes. When the queue is full, a
 * telemetry event replaces the queued telemetry of the same subscription
 * and nonzero coalesce_key, or else evicts the oldest queued telemetry; both
 * are counted, and the consumer sees the loss as a gap in the subscription's
 * sequence (ADR-0015).
 */
#define AYNI_EVENT_OBSERVATION ((ayni_event_kind_t)1)
#define AYNI_EVENT_TELEMETRY ((ayni_event_kind_t)2)
#define AYNI_EVENT_CLOCK_TRUST ((ayni_event_kind_t)4) /* absolute time became trusted */

#ifdef __cplusplus
}
#endif

#endif /* AYNI_DRAFT_KERNEL_API_H */
