// Kernel composition of memory ownership, captured authority and durable visibility.
#ifndef AYNI_CORE_KERNEL_MEMORY_ACCESS_H
#define AYNI_CORE_KERNEL_MEMORY_ACCESS_H
#include "kernel/service/model/work_model.h"
#include <ayni/memory.h>
namespace ayni::kernel {
// Synchronous owner-thread sample; no caller callback or mutable cache.
struct MemoryAccess {
    const WorkModel &model; // Authoritative memory, work and Grant owners.
    uint64_t now_us;        // Fresh monotonic authorization time in this boot.
    uint64_t durable_seq;   // Current writer's acknowledged prefix.
};
// Shared typed query validation for management and decision-scoped search.
ayni_status_t memory_search_form(const ayni_memory_search_v1 &query);
// Management authentication is identical for queries, confirmation and writes.
bool memory_management(const WorkModel &model, uint64_t session_id, uint64_t durable_seq);
// Model origin must be this Task's completed durable model call; derived content has no call claim.
bool memory_origin_matches(const WorkModel &model, uint64_t task_id, knowledge::MemorySource source,
                           uint64_t call_id, uint64_t durable_seq);
// Exact captured Grants and source revisions, including every transitive ancestor.
bool memory_current(const MemoryAccess &access, uint32_t index);
bool memory_dependencies_current(const MemoryAccess &access,
                                 const knowledge::MemoryDependencies &dependencies,
                                 uint64_t forbidden_id = 0);
// Existence/namespace relation, independent of the caller's authorization.
bool memory_namespace_exists(const WorkModel &model, knowledge::MemoryNamespace space, uint64_t id);
// Check Goal input and accumulated Task content without confusing privacy with action outcome.
bool task_content_current(const MemoryAccess &access, uint32_t task_index);
// Copied metadata never gives out table-owned body/label bytes.
ayni_memory_info_v1 memory_info(const MemoryAccess &access, uint32_t index);
} // namespace ayni::kernel
#endif
