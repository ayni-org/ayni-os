#include "kernel/service/memory/memory_access.h"
#include "catalog/abi_checks.h"
#include "foundation/utf8.h"
#include "kernel/service/memory/context_service.h"
namespace ayni::kernel {
ayni_reason_t memory_reason(ayni_status_t status) {
    return status == AYNI_DENIED              ? AYNI_REASON_MEMORY_SOURCE
           : status == AYNI_EXPIRED           ? AYNI_REASON_MEMORY_EXPIRED
           : status == AYNI_CAPACITY_EXCEEDED ? AYNI_REASON_MEMORY_DEPENDENCIES
                                              : AYNI_REASON_NONE;
}

namespace {
bool captured_grant_current(const MemoryAccess &access, knowledge::MemoryGrantRef ref,
                            uint64_t actor) {
    if (ref.id == 0) {
        return ref.revision == 0;
    }
    uint32_t index = 0;
    return access.model.grants.find(ref.id, index) &&
           access.model.grants.at(index).revision == ref.revision &&
           access.model.grants.at(index).subject_instance_id == actor &&
           access.model.check_grant(index, access.now_us, access.durable_seq).status == AYNI_OK;
}
bool dependency_leaf_current(const void *context, const knowledge::MemoryDependency &dependency) {
    const auto &access = *static_cast<const MemoryAccess *>(context);
    if (dependency.kind == knowledge::DependencyKind::context) {
        uint32_t index = 0;
        return access.model.contexts.current_reference(dependency, index) &&
               access.model.contexts.at(index).stamp.commit_seq <= access.durable_seq;
    }
    uint32_t source = 0;
    if (!access.model.memories.find(dependency.id, source)) {
        return false;
    }
    const auto &record = access.model.memories.at(source);
    if (dependency.read.id == 0 && (record.space != knowledge::MemoryNamespace::private_agent ||
                                    record.namespace_id != dependency.actor_instance_id)) {
        return false;
    }
    return captured_grant_current(access, dependency.read, dependency.actor_instance_id) &&
           captured_grant_current(access, dependency.egress, dependency.actor_instance_id);
}
bool dependency_current(const void *context, const knowledge::MemoryDependency &dependency) {
    const auto &access = *static_cast<const MemoryAccess *>(context);
    if (dependency.kind != knowledge::DependencyKind::context) {
        return dependency_leaf_current(context, dependency);
    }
    uint32_t index = 0;
    return access.model.contexts.current_reference(dependency, index) &&
           context_current(access, index);
}
} // namespace
bool context_current(const MemoryAccess &access, uint32_t index) {
    const auto &record = access.model.contexts.at(index);
    // The stored set is the complete captured closure. Context leaves check exact revisions,
    // avoiding recursion through a memory later organized from this same conversation.
    return record.pending.action <= knowledge::ContextAction::publish &&
           record.stamp.commit_seq <= access.durable_seq &&
           access.model.memories.dependencies_current(record.dependencies, access.durable_seq,
                                                      {&access, dependency_leaf_current});
}
ayni_status_t memory_search_form(const ayni_memory_search_v1 &q) {
    const auto shape = ayni::catalog::check_struct_size(q.struct_size, sizeof(q), sizeof(q));
    if (shape != AYNI_OK) {
        return shape;
    }
    if ((q.name_space != AYNI_MEMORY_PRIVATE && q.name_space != AYNI_MEMORY_PROFILE) ||
        q.namespace_id == 0 || q.keyword.size > AYNI_MEMORY_BODY_BYTES_MAX ||
        q.tag.size > AYNI_MEMORY_LABEL_BYTES_MAX ||
        !ayni::foundation::valid_utf8(q.keyword.data, q.keyword.size) ||
        !ayni::foundation::valid_utf8(q.tag.data, q.tag.size)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
}
bool memory_management(const WorkModel &model, uint64_t session_id, uint64_t durable_seq) {
    const auto *session = model.sessions.find(session_id);
    return session != nullptr && bound_management_session(model, *session, durable_seq);
}
bool memory_origin_matches(const WorkModel &model, uint64_t task_id, knowledge::MemorySource source,
                           uint64_t call_id, uint64_t durable_seq) {
    if (source == knowledge::MemorySource::derived) {
        return call_id == 0;
    }
    uint32_t index = 0;
    return source == knowledge::MemorySource::model && model.calls.find(call_id, index) &&
           model.calls.identity(index).task_id == task_id &&
           model.calls.contract(index).category == catalog::CallClass::model &&
           model.calls.state(index).outcome == AYNI_CALL_COMPLETED &&
           model.calls.state(index).latest.transition.commit_seq <= durable_seq;
}
bool memory_current(const MemoryAccess &access, uint32_t index) {
    return access.model.memories.current(index, access.durable_seq, {&access, dependency_current});
}
bool memory_dependencies_current(const MemoryAccess &access,
                                 const knowledge::MemoryDependencies &dependencies,
                                 uint64_t forbidden_id) {
    return access.model.memories.dependencies_current(dependencies, access.durable_seq,
                                                      {&access, dependency_current}, forbidden_id);
}
bool memory_namespace_exists(const WorkModel &model, knowledge::MemoryNamespace space,
                             uint64_t id) {
    if (space == knowledge::MemoryNamespace::private_agent) {
        return model.agents.find_instance(id) != nullptr;
    }
    const auto *owner = model.principals.binding();
    return space == knowledge::MemoryNamespace::owner_profile && owner != nullptr &&
           owner->principal_id == id;
}
bool goal_content_current(const MemoryAccess &access, uint32_t goal_index) {
    if (!goal_content_compatible(access.model, goal_index)) {
        return false;
    }
    const auto &goal = access.model.goals.at(goal_index);
    const auto *source =
        access.model.provenance.find(work::ProvenanceKind::goal, goal_index, goal.goal_id);
    return source == nullptr || (source->content == work::ContentState::available &&
                                 memory_dependencies_current(access, source->dependencies));
}
bool task_content_current(const MemoryAccess &access, uint32_t task_index) {
    const auto &task = access.model.tasks.at(task_index);
    if (!goal_content_current(access, task.goal_index)) {
        return false;
    }
    const auto *source =
        access.model.provenance.find(work::ProvenanceKind::task, task.slot, task.task_id);
    return source == nullptr || (source->content == work::ContentState::available &&
                                 memory_dependencies_current(access, source->dependencies));
}
ayni_memory_info_v1 memory_info(const MemoryAccess &access, uint32_t index) {
    const auto &record = access.model.memories.at(index);
    return {sizeof(ayni_memory_info_v1),
            record.revision,
            record.id,
            record.namespace_id,
            static_cast<ayni_memory_namespace_t>(record.space),
            static_cast<ayni_profile_field_t>(record.profile_field),
            static_cast<ayni_memory_source_t>(record.source),
            static_cast<ayni_memory_state_t>(record.state),
            record.confirmed ? 1U : 0U,
            memory_current(access, index) ? 1U : 0U,
            record.body_size,
            record.dependencies.count,
            record.source_call_id,
            record.stamp.commit_seq};
}
} // namespace ayni::kernel
