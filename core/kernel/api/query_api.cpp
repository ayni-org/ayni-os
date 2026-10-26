// Bounded copying management queries (ADR-0057). Tables retain ownership of
// their facts; this entry exposes stable IDs without leaking slots or pointers.
#include "kernel/api/api_gate.h"
#include "kernel/kernel_state.h"

namespace {
// Check the whole output bound before publishing any ID. No partial listing is
// reported as complete, and empty stores permit a NULL zero-capacity buffer.
template <typename Id>
ayni_status_t copy_ids(uint32_t count, uint64_t *ids, uint32_t capacity, uint32_t *out_count,
                       Id id) {
    if (capacity < count) {
        *out_count = count;
        return AYNI_BUFFER_TOO_SMALL;
    }
    for (uint32_t at = 0; at < count; ++at) {
        ids[at] = id(at);
    }
    *out_count = count;
    return AYNI_OK;
}
} // namespace

// A call table has holes after Task retirement. Its accessor advances over
// them once; dense tables use their existing read-only indexed accessors.
ayni_status_t ayni_list_objects(const ayni_kernel_t *kernel, ayni_object_kind_t kind, uint64_t *ids,
                                uint32_t capacity, uint32_t *out_count) {
    if (!kernel || !out_count || (capacity && !ids)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (kind != AYNI_OBJECT_GOAL && kind != AYNI_OBJECT_TASK && kind != AYNI_OBJECT_CALL &&
        kind != AYNI_OBJECT_GRANT) {
        return AYNI_UNSUPPORTED;
    }
    if (kernel->in_callback()) {
        return AYNI_INVALID_STATE;
    }
    const auto gated = ayni::kernel::admit(*kernel, ayni::kernel::gate::query);
    if (gated != AYNI_OK) {
        return gated;
    }
    const auto &work = kernel->work;
    switch (kind) {
    case AYNI_OBJECT_GOAL:
        return copy_ids(work.goals.size(), ids, capacity, out_count,
                        [&](uint32_t at) { return work.goals.at(at).goal_id; });
    case AYNI_OBJECT_TASK:
        return copy_ids(work.tasks.size(), ids, capacity, out_count,
                        [&](uint32_t at) { return work.tasks.at(at).task_id; });
    case AYNI_OBJECT_GRANT:
        return copy_ids(work.grants.size(), ids, capacity, out_count,
                        [&](uint32_t at) { return work.grants.at(at).grant_id; });
    default: {
        uint32_t slot = 0;
        return copy_ids(work.calls.size(), ids, capacity, out_count, [&](uint32_t) {
            while (!work.calls.occupied(slot)) {
                ++slot;
            }
            return work.calls.identity(slot++).call_id;
        });
    }
    }
}
