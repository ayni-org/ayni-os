#include "kernel/service/memory/context_service.h"
namespace ayni::kernel {
ayni_status_t prepare_goal_content(const MemoryAccess &access, const catalog::GoalType &type,
                                   uint64_t owner, GoalContent &out) {
    if (type.context_mode == catalog::GoalContext::none) {
        out = {};
        return AYNI_OK;
    }
    if (access.model.contexts.capacity() == 0) {
        return AYNI_UNSUPPORTED;
    }
    uint32_t index = 0;
    if (!access.model.contexts.find_owner(owner, index)) {
        return AYNI_NOT_READY;
    }
    const auto &context = access.model.contexts.at(index);
    if (context.pending.action != knowledge::ContextAction::none ||
        context.stamp.commit_seq > access.durable_seq) {
        return AYNI_BUSY;
    }
    if (!context_current(access, index)) {
        return AYNI_EXPIRED;
    }
    GoalContent candidate{context.dependencies, {context.id, context.generation}};
    knowledge::MemoryDependencies anchor{};
    anchor.items[0] = {knowledge::DependencyKind::context,
                       context.id,
                       context.revision,
                       owner,
                       knowledge::MemoryPurpose::context,
                       {},
                       {}};
    anchor.count = 1;
    const auto status = knowledge::merge_dependencies(candidate.dependencies, anchor,
                                                      access.model.memories.limits().dependencies);
    if (status == AYNI_OK) {
        out = candidate;
    }
    return status;
}
// Source metadata includes the complete closure, so removing only the primary anchor does not
// compress memory lineage. The exact file generation prevents publishing an old Task twice.
bool task_context_sources(const WorkModel &model, uint32_t task_index,
                          const knowledge::ContextRecord &context,
                          knowledge::MemoryDependencies &out) {
    const auto &task = model.tasks.at(task_index);
    const auto *goal =
        model.provenance.find(work::ProvenanceKind::goal, task.goal_index, task.goal_id);
    const auto *sources =
        model.provenance.find(work::ProvenanceKind::task, task.slot, task.task_id);
    if (task.owner_instance_id != context.owner_instance_id ||
        task.lifecycle != AYNI_TASK_COMPLETED || goal == nullptr || sources == nullptr ||
        goal->content != work::ContentState::available ||
        sources->content != work::ContentState::available || goal->origin.id != context.id ||
        goal->origin.generation != context.generation) {
        return false;
    }
    knowledge::MemoryDependencies candidate{};
    bool own_anchor = false;
    for (uint32_t index = 0; index < sources->dependencies.count; ++index) {
        const auto &ref = sources->dependencies.items[index];
        if (ref.kind == knowledge::DependencyKind::context && ref.id == context.id) {
            if (ref.revision != context.revision ||
                ref.actor_instance_id != context.owner_instance_id) {
                return false;
            }
            own_anchor = true;
        } else {
            candidate.items[candidate.count++] = ref;
        }
    }
    if (!own_anchor) {
        return false;
    }
    out = candidate;
    return true;
}
bool goal_content_compatible(const WorkModel &model, uint32_t goal_index) {
    const auto &goal = model.goals.at(goal_index);
    if (goal.goal_type_index == work::GoalTable::no_index ||
        model.catalogs.goal_types.at(goal.goal_type_index).context_mode ==
            catalog::GoalContext::none) {
        return true;
    }
    const auto *sources =
        model.provenance.find(work::ProvenanceKind::goal, goal_index, goal.goal_id);
    return sources != nullptr &&
           (sources->content == work::ContentState::erased || sources->origin.id != 0);
}
bool task_content_compatible(const WorkModel &model, uint32_t task_index) {
    return goal_content_compatible(model, model.tasks.at(task_index).goal_index);
}
} // namespace ayni::kernel
