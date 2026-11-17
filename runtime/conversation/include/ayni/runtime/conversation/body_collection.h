// Narrow body/collection port used by a conversation; implementations own their caches and I/O.
#ifndef AYNI_RUNTIME_BODY_COLLECTION_H
#define AYNI_RUNTIME_BODY_COLLECTION_H
#include <ayni/bodies.h>
#include <vector>
namespace ayni::runtime {
// Views live through Host join. collect is owner-thread and non-reentrant; asynchronous
// cleanup provides collected. It deletes unreferenced scope bodies, never merely evicts bytes.
struct BodyCollection {
    const ayni_body_store_v1 *store = nullptr; // Borrowed stable descriptor; null is unconfigured.
    void *context = nullptr; // Implementation-owned callback state through Host join.
    void (*collect)(void *, uint64_t, const std::vector<ayni_body_ref_v1> &) = nullptr;
    // Optional owner progress/global drain callback; null means no store-wide drain is needed.
    uint32_t (*ready)(void *, uint64_t) = nullptr;
    // Sticky background I/O status; null means the implementation has no separate failure lane.
    ayni_status_t (*status)(void *) = nullptr;
    // Completion of releases already requested for this scope; null means synchronous cleanup.
    // Unrelated scopes and subsequent writes cannot delay a completed ownership removal.
    // Owner-thread, non-reentrant: OK is durable completion, BUSY is pending; errors propagate.
    ayni_status_t (*collected)(void *, uint64_t scope) = nullptr;
    // Optional progress without the global drain query used for Host shutdown.
    uint32_t (*progress)(void *, uint64_t) = nullptr;
};
} // namespace ayni::runtime
#endif
