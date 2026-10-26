// Kernel composition of a file anchor with work sources; no file I/O is performed here.
#ifndef AYNI_CORE_KERNEL_MEMORY_CONTEXT_SERVICE_H
#define AYNI_CORE_KERNEL_MEMORY_CONTEXT_SERVICE_H
#include "kernel/service/memory/memory_access.h"
namespace ayni::kernel {
// Complete source candidate for one new Goal. Empty origin means independent input.
struct GoalContent {
    knowledge::MemoryDependencies
        dependencies{};           // Complete captured closure, including own anchor.
    work::ContextOrigin origin{}; // Primary file basis; never inherited from an unrelated Goal.
};
// Trusted GoalType selects whether input must use its Agent's current, acknowledged file basis.
ayni_status_t prepare_goal_content(const MemoryAccess &access, const catalog::GoalType &type,
                                   uint64_t owner, GoalContent &out);
// Completed Task's exact publication sources. Historical proof does not consult clock or catalog.
bool task_context_sources(const WorkModel &model, uint32_t task_index,
                          const knowledge::ContextRecord &context,
                          knowledge::MemoryDependencies &out);
// Compatibility guard for old chat work whose input has no primary source evidence.
bool goal_content_compatible(const WorkModel &model, uint32_t goal_index);
bool task_content_compatible(const WorkModel &model, uint32_t task_index);
} // namespace ayni::kernel
#endif
