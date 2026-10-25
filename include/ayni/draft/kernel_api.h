/*
 * DRAFT, not part of the published ABI (C-VER-007): no symbols; calling these
 * functions fails at link time instead of returning a fabricated result.
 *
 * Planned kernel API beyond ayni.h: catalog registration, further intents and
 * events (plan section 3.2; contracts/domain/lifecycles.md). Implemented
 * functions and types stay in published headers and are not redeclared.
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
#include <ayni/calls.h>
#include <ayni/resources.h>

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

/* Resource registration lives in ayni/resources.h since 0.29. Control-domain
 * authority epochs and device/binding associations remain MA-018B/MA-008;
 * the current ACTUATOR flag promises quarantine, not execution permission. */

/* Call execution properties, Provider offers and Binding registration are
 * published in ayni/calls.h since 0.31. Device registration remains a draft. */
ayni_status_t ayni_register_device(ayni_kernel_t *kernel, const ayni_device_desc_v1 *device);

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
/* Draft-only values are deliberately outside the implemented range. The old
 * draft 11/12 aliased published Goal approval/reauthorization; no draft body
 * may be interpreted as an implemented command (ADR-0042). Assign final
 * published values only when implementing each typed contract. */
#define AYNI_INTENT_MEMORY_WRITE ((ayni_intent_kind_t)0x10001u)
#define AYNI_INTENT_MEMORY_DELETE ((ayni_intent_kind_t)0x10002u)
#define AYNI_INTENT_SUBSCRIPTION_OPEN ((ayni_intent_kind_t)0x10003u)
#define AYNI_INTENT_SUBSCRIPTION_CLOSE ((ayni_intent_kind_t)0x10004u)
/* ADR-0042: Core owns a generic persistent control group; the trusted assistant
 * classifies work. These application-oriented draft intents remain unsupported. */
#define AYNI_INTENT_ASSISTANT_PAUSE ((ayni_intent_kind_t)0x10005u)
#define AYNI_INTENT_ASSISTANT_RESUME ((ayni_intent_kind_t)0x10006u)

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
