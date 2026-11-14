// Narrow body/collection port used by a conversation; implementations own their caches and I/O.
#ifndef AYNI_RUNTIME_BODY_COLLECTION_H
#define AYNI_RUNTIME_BODY_COLLECTION_H
#include <ayni/bodies.h>
#include <vector>
namespace ayni::runtime {
// Views live through Host join. collect is owner-thread, non-reentrant and asynchronous;
// it deletes unreferenced bodies of this scope, never merely evicts cached bytes.
struct BodyCollection {
    const ayni_body_store_v1 *store = nullptr; // Borrowed stable descriptor; null is unconfigured.
    void *context = nullptr; // Implementation-owned callback state through Host join.
    void (*collect)(void *, uint64_t, const std::vector<ayni_body_ref_v1> &) = nullptr;
    // Optional owner progress/drain callback; null means no asynchronous body work.
    uint32_t (*ready)(void *, uint64_t) = nullptr;
    // Sticky background I/O status; null means the implementation has no separate failure lane.
    ayni_status_t (*status)(void *) = nullptr;
};
} // namespace ayni::runtime
#endif
