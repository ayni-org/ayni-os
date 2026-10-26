#include "kernel/service/memory/memory_permissions.h"
#include "authority/scope/scope.h"
#include "foundation/kernel_id.h"
namespace ayni::kernel {
namespace {
constexpr char access_name[] = "memory.access";
constexpr ayni_string_v1 access_capability{access_name, sizeof(access_name) - 1};
constexpr char export_name[] = "memory.egress";
constexpr ayni_string_v1 export_capability{export_name, sizeof(export_name) - 1};
constexpr char id_name[] = "id";
constexpr ayni_string_v1 id_unit{id_name, sizeof(id_name) - 1};
constexpr char code_name[] = "code";
constexpr ayni_string_v1 code_unit{code_name, sizeof(code_name) - 1};
ayni_scope_field_v1 number(uint32_t id, int64_t value, ayni_string_v1 unit) {
    ayni_scope_field_v1 field{};
    field.struct_size = sizeof(field);
    field.field_id = id;
    field.allowed.struct_size = sizeof(field.allowed);
    field.allowed.kind = AYNI_AUTHORITY_I64;
    field.allowed.unit = unit;
    field.allowed.values.integer.min_inclusive = value;
    field.allowed.values.integer.max_inclusive = value;
    return field;
}
// One bounded request uses source ID parts and exact purpose, never an owner-wide implicit scope.
std::array<ayni_scope_field_v1, memory_scope_fields>
source_fields(uint64_t id, knowledge::MemoryPurpose purpose) {
    return {
        number(memory_epoch_field, static_cast<int64_t>(foundation::id_boot_epoch(id)), id_unit),
        number(memory_sequence_field, foundation::id_sequence(id), id_unit),
        number(memory_purpose_field, static_cast<int64_t>(purpose), code_unit),
        {}};
}
// The retained source domain owns interpretation; runtime catalog changes cannot broaden old
// grants.
bool covers(const MemoryAccess &access, uint32_t index, uint64_t actor, ayni_string_v1 capability,
            const ayni_scope_v1 &request) {
    const auto &grants = access.model.grants;
    const auto &grant = grants.at(index);
    if (grant.subject_instance_id != actor || !grant.capability_id.equals(capability) ||
        grant.capability_version != memory_capability_version ||
        access.model.check_grant(index, access.now_us, access.durable_seq).status != AYNI_OK) {
        return false;
    }
    const auto match =
        authority::contains_request(grants.source_domain(index), grants.scope(index), request);
    return match.status == AYNI_OK && match.covered;
}
ayni_status_t select_permission(const MemoryAccess &access, uint64_t actor,
                                ayni_string_v1 capability, const ayni_scope_v1 &request,
                                knowledge::MemoryGrantRef &ref) {
    uint32_t selected = access.model.grants.size();
    for (uint32_t i = 0; i < access.model.grants.size(); ++i) {
        const auto &g = access.model.grants.at(i);
        if ((ref.id != 0 && (g.grant_id != ref.id || g.revision != ref.revision)) ||
            !covers(access, i, actor, capability, request)) {
            continue;
        }
        if (selected == access.model.grants.size() ||
            g.grant_id < access.model.grants.at(selected).grant_id) {
            selected = i;
        }
    }
    if (selected == access.model.grants.size()) {
        return AYNI_DENIED;
    }
    const auto &g = access.model.grants.at(selected);
    ref = {g.grant_id, g.revision};
    return AYNI_OK;
}
// This checks the immutable meaning of a captured Grant, not whether history is still usable.
bool historical_permission(const WorkModel &model, const knowledge::MemoryDependency &source,
                           knowledge::MemoryGrantRef ref, ayni_string_v1 capability) {
    if (ref.id == 0) {
        return ref.revision == 0;
    }
    uint32_t index = 0;
    if (!model.grants.find(ref.id, index)) {
        return false;
    }
    const auto &grant = model.grants.at(index);
    if (grant.subject_instance_id != source.actor_instance_id || grant.revision < ref.revision ||
        !grant.capability_id.equals(capability) ||
        grant.capability_version != memory_capability_version) {
        return false;
    }
    auto fields = memory_read_fields(source.id, source.purpose);
    ayni_string_v1 destination{};
    if (capability.data != access_capability.data) {
        catalog::DomainReader reader(model.grants.scope(index));
        catalog::DomainField field{};
        while (reader.next(field)) {
            if (field.id == memory_destination_field &&
                field.values.kind == AYNI_AUTHORITY_IDENTIFIER && field.values.token_count != 0) {
                destination = field.values.tokens[0];
            }
        }
        if (!reader.ok() || destination.size == 0) {
            return false;
        }
        fields = memory_export_fields(source.id, source.purpose, &destination);
    }
    const auto covered =
        authority::contains_request(model.grants.source_domain(index), model.grants.scope(index),
                                    {sizeof(ayni_scope_v1), memory_scope_fields, fields.data()});
    return covered.status == AYNI_OK && covered.covered;
}
} // namespace
std::array<ayni_scope_field_v1, memory_scope_fields>
memory_read_fields(uint64_t id, knowledge::MemoryPurpose purpose) {
    auto fields = source_fields(id, purpose);
    fields.back() = number(memory_operation_field, memory_read_operation, code_unit);
    return fields;
}
std::array<ayni_scope_field_v1, memory_scope_fields>
memory_export_fields(uint64_t id, knowledge::MemoryPurpose purpose,
                     const ayni_string_v1 *destination) {
    auto fields = source_fields(id, purpose);
    auto &field = fields.back();
    field.struct_size = sizeof(field);
    field.field_id = memory_destination_field;
    field.allowed.struct_size = sizeof(field.allowed);
    field.allowed.kind = AYNI_AUTHORITY_IDENTIFIER;
    field.allowed.values.tokens = {destination, 1, 0};
    return fields;
}
bool memory_purpose_allowed(knowledge::MemoryNamespace space, uint64_t namespace_id, uint64_t actor,
                            knowledge::MemoryPurpose purpose) {
    return space != knowledge::MemoryNamespace::private_agent || namespace_id == actor ||
           purpose != knowledge::MemoryPurpose::organize;
}
bool memory_dependencies_history(const WorkModel &model,
                                 const knowledge::MemoryDependencies &dependencies,
                                 uint64_t boot_epoch_limit, uint64_t actor) {
    for (uint32_t i = 0; i < dependencies.count; ++i) {
        const auto &ref = dependencies.items[i];
        if (foundation::id_boot_epoch(ref.id) > boot_epoch_limit ||
            foundation::id_boot_epoch(ref.actor_instance_id) > boot_epoch_limit ||
            foundation::id_boot_epoch(ref.read.id) > boot_epoch_limit ||
            foundation::id_boot_epoch(ref.egress.id) > boot_epoch_limit ||
            model.agents.find_instance(ref.actor_instance_id) == nullptr) {
            return false;
        }
        if (ref.kind == knowledge::DependencyKind::context) {
            uint32_t index = 0;
            if (!model.contexts.find(ref.id, index) ||
                model.contexts.at(index).owner_instance_id != ref.actor_instance_id ||
                model.contexts.at(index).revision < ref.revision) {
                return false;
            }
            continue;
        }
        if ((actor != 0 && actor != ref.actor_instance_id) ||
            !historical_permission(model, ref, ref.read, access_capability) ||
            !historical_permission(model, ref, ref.egress, export_capability)) {
            return false;
        }
        uint32_t source = 0;
        if (model.memories.find(ref.id, source) &&
            !memory_purpose_allowed(model.memories.at(source).space,
                                    model.memories.at(source).namespace_id, ref.actor_instance_id,
                                    ref.purpose)) {
            return false;
        }
        if (ref.read.id == 0 && model.memories.find(ref.id, source) &&
            (model.memories.at(source).space != knowledge::MemoryNamespace::private_agent ||
             model.memories.at(source).namespace_id != ref.actor_instance_id)) {
            return false;
        }
    }
    return true;
}
ayni_status_t memory_read_permission(const MemoryAccess &access, uint64_t actor,
                                     uint32_t record_index, knowledge::MemoryPurpose purpose,
                                     knowledge::MemoryGrantRef &out) {
    if (purpose != knowledge::MemoryPurpose::context &&
        purpose != knowledge::MemoryPurpose::organize) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &record = access.model.memories.at(record_index);
    if (record.space == knowledge::MemoryNamespace::private_agent) {
        if (record.namespace_id == actor) {
            out = {};
            return AYNI_OK;
        }
        // C-PROD-042: background organization never enters another Agent's private namespace.
        if (!memory_purpose_allowed(record.space, record.namespace_id, actor, purpose)) {
            return AYNI_DENIED;
        }
    }
    auto fields = memory_read_fields(record.id, purpose);
    knowledge::MemoryGrantRef selected{};
    const auto status =
        select_permission(access, actor, access_capability,
                          {sizeof(ayni_scope_v1), memory_scope_fields, fields.data()}, selected);
    if (status == AYNI_OK) {
        out = selected;
    }
    return status;
}
// Mutate only a complete staged copy: any missing permission leaves the original lineage unchanged.
ayni_status_t memory_export_permissions(const MemoryAccess &access, uint64_t actor,
                                        const catalog::Binding &binding,
                                        knowledge::MemoryDependencies &dependencies) {
    if (binding.destination.size() == 0) {
        return AYNI_OK;
    }
    const ayni_string_v1 destination{binding.destination.data(), binding.destination.size()};
    auto staged = dependencies;
    for (uint32_t i = 0; i < staged.count; ++i) {
        auto &dependency = staged.items[i];
        if (dependency.kind != knowledge::DependencyKind::memory) {
            continue;
        }
        if (dependency.actor_instance_id != actor) {
            return AYNI_DENIED;
        }
        auto fields = memory_export_fields(dependency.id, dependency.purpose, &destination);
        const auto status = select_permission(
            access, actor, export_capability,
            {sizeof(ayni_scope_v1), memory_scope_fields, fields.data()}, dependency.egress);
        if (status != AYNI_OK) {
            return status;
        }
    }
    dependencies = staged;
    return AYNI_OK;
}
// Dispatch consumes the exact permissions published with the checkpoint/call decision.
// A newly issued broader grant cannot repair an expired or absent captured export permission.
bool task_export_current(const MemoryAccess &access, uint32_t task_index,
                         const catalog::Binding &binding) {
    if (!task_content_current(access, task_index)) {
        return false;
    }
    if (binding.destination.size() == 0) {
        return true;
    }
    const auto &task = access.model.tasks.at(task_index);
    const auto *goal =
        access.model.provenance.find(work::ProvenanceKind::goal, task.goal_index, task.goal_id);
    const auto *saved =
        access.model.provenance.find(work::ProvenanceKind::task, task.slot, task.task_id);
    knowledge::MemoryDependencies dependencies{};
    constexpr std::size_t source_owners = 2;
    for (const auto *source :
         std::array<const work::ProvenanceRecord *, source_owners>{goal, saved}) {
        if (source != nullptr &&
            knowledge::merge_dependencies(dependencies, source->dependencies,
                                          access.model.memories.limits().dependencies) != AYNI_OK) {
            return false;
        }
    }
    for (uint32_t i = 0; i < dependencies.count; ++i) {
        if (dependencies.items[i].kind == knowledge::DependencyKind::memory &&
            dependencies.items[i].egress.id == 0) {
            return false;
        }
    }
    return memory_export_permissions(access, task.owner_instance_id, binding, dependencies) ==
           AYNI_OK;
}
} // namespace ayni::kernel
