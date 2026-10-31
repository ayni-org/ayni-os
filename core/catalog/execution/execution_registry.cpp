#include "catalog/execution/execution_registry.h"
#include "catalog/abi_checks.h"
#include "catalog/capability/business_encoding.h"
#include <cstring>
namespace ayni::catalog {
namespace {
ayni_string_v1 view(const CatalogId &id) {
    return {id.data(), id.size()};
}

bool valid_field(const DomainField &field, const ExecutionField &mapping) {
    return field.values.kind == AYNI_AUTHORITY_I64 && mapping.unit.equals(field.values.unit);
}
// Resolve bounded numeric projection once for each complete domain/request. A
// concrete request supplies one integer; a static domain may declare an interval.
bool read_projection(const ExecutionBinding &binding, ayni_bytes_v1 domain,
                     std::array<int64_t, AYNI_CONTROL_FIELD_COUNT_MAX> &minima,
                     std::array<int64_t, AYNI_CONTROL_FIELD_COUNT_MAX> &maxima, bool &concrete) {
    std::array<bool, AYNI_CONTROL_FIELD_COUNT_MAX> lower{};
    std::array<bool, AYNI_CONTROL_FIELD_COUNT_MAX> upper{};
    DomainReader reader(domain);
    DomainField field{};
    concrete = true;
    while (reader.next(field)) {
        for (uint32_t at = 0; at < binding.field_count; ++at) {
            const auto &mapping = binding.fields[at];
            if (field.id != mapping.minimum_field && field.id != mapping.maximum_field) {
                continue;
            }
            if (!valid_field(field, mapping)) {
                return false;
            }
            concrete = concrete && field.values.minimum == field.values.maximum;
            if (field.id == mapping.minimum_field) {
                lower[at] = true;
                minima[at] = field.values.minimum;
            }
            if (field.id == mapping.maximum_field) {
                upper[at] = true;
                maxima[at] = field.values.maximum;
            }
        }
    }
    if (!reader.ok()) {
        return false;
    }
    for (uint32_t at = 0; at < binding.field_count; ++at) {
        if (!lower[at] || !upper[at]) {
            return false;
        }
    }
    return true;
}
bool actuator(const CallContract &contract, const ResourceRegistry &resources) {
    bool found = false;
    for (uint32_t at = 0; at < contract.resource_count; ++at) {
        uint32_t index = 0;
        if (!resources.find(view(contract.resources[at].id), index)) {
            return false;
        }
        const auto &resource = resources.at(index);
        if ((resource.flags & AYNI_RESOURCE_ACTUATOR) != 0) {
            if (resource.kind != AYNI_RESOURCE_EXCLUSIVE) {
                return false;
            }
            found = true;
        }
    }
    return found;
}
} // namespace
ayni_status_t validate_execution_binding(const ExecutionBinding &binding) {
    if (binding.binding.size() == 0 || binding.binding_version == 0 ||
        binding.executor.size() == 0 || binding.executor_version == 0 ||
        binding.scene.size() == 0 || binding.scene_version == 0 ||
        (binding.form != AYNI_FORM_PROCEDURE && binding.form != AYNI_FORM_SESSION) ||
        binding.lease_us == 0 || binding.max_command_age_us == 0 || binding.field_count == 0 ||
        binding.field_count > binding.fields.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < binding.field_count; ++at) {
        const auto &field = binding.fields[at];
        if (field.id.size() == 0 || field.unit.size() == 0 || field.minimum_field == 0 ||
            field.maximum_field == 0 ||
            (at > 0 && binding.fields[at - 1].id.compare(field.id) >= 0)) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    return AYNI_OK;
}
bool same_execution_binding(const ExecutionBinding &left, const ExecutionBinding &right) {
    if (!left.binding.equals(view(right.binding)) ||
        left.binding_version != right.binding_version ||
        !left.executor.equals(view(right.executor)) ||
        left.executor_version != right.executor_version || !left.scene.equals(view(right.scene)) ||
        left.scene_version != right.scene_version || left.form != right.form ||
        left.lease_us != right.lease_us || left.max_command_age_us != right.max_command_age_us ||
        left.field_count != right.field_count) {
        return false;
    }
    for (uint32_t at = 0; at < left.field_count; ++at) {
        const auto &a = left.fields[at];
        const auto &b = right.fields[at];
        if (!a.id.equals(view(b.id)) || !a.unit.equals(view(b.unit)) ||
            a.minimum_field != b.minimum_field || a.maximum_field != b.maximum_field) {
            return false;
        }
    }
    return true;
}
ayni_status_t execution_envelope(const ExecutionBinding &binding, ayni_bytes_v1 arguments,
                                 ayni_control_envelope_v1 &out) {
    std::array<int64_t, AYNI_CONTROL_FIELD_COUNT_MAX> minima{};
    std::array<int64_t, AYNI_CONTROL_FIELD_COUNT_MAX> maxima{};
    bool concrete = false;
    if (!read_projection(binding, arguments, minima, maxima, concrete) || !concrete) {
        return AYNI_INVALID_ARGUMENT;
    }
    ayni_control_envelope_v1 envelope{};
    envelope.count = binding.field_count;
    for (uint32_t at = 0; at < binding.field_count; ++at) {
        if (minima[at] > maxima[at]) {
            return AYNI_INVALID_ARGUMENT;
        }
        envelope.fields[at] = {binding.fields[at].id.snapshot(), binding.fields[at].unit.snapshot(),
                               minima[at], maxima[at]};
    }
    out = envelope;
    return AYNI_OK;
}
std::size_t ExecutionRegistry::storage_size(uint32_t executors, uint32_t bindings) {
    return foundation::BoundedArray<Executor>::storage_size(executors) +
           foundation::BoundedArray<ExecutionBinding>::storage_size(bindings);
}
ExecutionRegistry::ExecutionRegistry(void *storage, uint32_t executors, uint32_t bindings)
    : executors_(storage, executors),
      bindings_(storage == nullptr
                    ? nullptr
                    : static_cast<uint8_t *>(storage) +
                          foundation::BoundedArray<Executor>::storage_size(executors),
                bindings) {}
ayni_status_t ExecutionRegistry::add_executor(const ayni_executor_v1 &input) {
    const auto shape = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (shape != AYNI_OK) {
        return shape;
    }
    Executor executor{};
    if (!executor.id.assign(input.executor_id) || input.version == 0 ||
        input.control_domain_id == 0 || input.executor_boot_epoch == 0 || input.safety_epoch == 0 ||
        input.calibration_version == 0 || input.control_mode == 0 || input.procedure_max_us == 0 ||
        input.session_lease_max_us == 0 || input.command_age_max_us == 0 ||
        input.poll_period_us == 0 || input.poll_period_us > input.procedure_max_us ||
        input.poll_period_us > input.session_lease_max_us || input.challenge == nullptr ||
        input.renew == nullptr || input.stop == nullptr || input.status == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t at = 0; at < executors_.size(); ++at) {
        if (executors_[at].id.equals(input.executor_id) ||
            executors_[at].ports.control_domain_id == input.control_domain_id) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    executor.ports = input;
    executor.ports
        .executor_id = {}; // The CatalogId owns the copied string, never a retained caller view.
    return executors_.push_back(executor) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
ayni_status_t ExecutionRegistry::add_binding(const ayni_execution_binding_v1 &input) {
    const auto shape = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (shape != AYNI_OK) {
        return shape;
    }
    ExecutionBinding binding{};
    if (!binding.binding.assign(input.binding_id) || !binding.executor.assign(input.executor_id) ||
        !binding.scene.assign(input.scene_id) || input.fields == nullptr ||
        input.field_count == 0 || input.field_count > binding.fields.size()) {
        return AYNI_INVALID_ARGUMENT;
    }
    binding.binding_version = input.binding_version;
    binding.executor_version = input.executor_version;
    binding.scene_version = input.scene_version;
    binding.form = input.form;
    binding.lease_us = input.lease_us;
    binding.max_command_age_us = input.max_command_age_us;
    binding.field_count = input.field_count;
    for (uint32_t at = 0; at < input.field_count; ++at) {
        ExecutionField field{};
        if (!field.id.assign(input.fields[at].field_id) ||
            !field.unit.assign(input.fields[at].unit)) {
            return AYNI_INVALID_ARGUMENT;
        }
        field.minimum_field = input.fields[at].minimum_field_id;
        field.maximum_field = input.fields[at].maximum_field_id;
        uint32_t insert = at;
        while (insert > 0 && binding.fields[insert - 1].id.compare(field.id) > 0) {
            binding.fields[insert] = binding.fields[insert - 1];
            --insert;
        }
        binding.fields[insert] = field;
    }
    const auto status = validate_execution_binding(binding);
    if (status != AYNI_OK) {
        return status;
    }
    uint32_t duplicate = 0;
    if (find_binding(input.binding_id, input.binding_version, duplicate)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return bindings_.push_back(binding) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
bool ExecutionRegistry::find_executor(ayni_string_v1 id, uint32_t version, uint32_t &index) const {
    for (uint32_t at = 0; at < executors_.size(); ++at) {
        if (executors_[at].id.equals(id) && executors_[at].ports.version == version) {
            index = at;
            return true;
        }
    }
    return false;
}
bool ExecutionRegistry::find_binding(ayni_string_v1 id, uint32_t version, uint32_t &index) const {
    for (uint32_t at = 0; at < bindings_.size(); ++at) {
        if (bindings_[at].binding.equals(id) && bindings_[at].binding_version == version) {
            index = at;
            return true;
        }
    }
    return false;
}
// Catalog validation checks typed parameter sources and finite executor limits.
// Authority owns Scene predicates/enabling, so that relation is checked by Kernel.
ayni_status_t ExecutionRegistry::validate_all(const BindingRegistry &bindings,
                                              const CapabilityRegistry &capabilities,
                                              const CallCapabilityRegistry &calls,
                                              const ResourceRegistry &resources) const {
    for (uint32_t at = 0; at < bindings_.size(); ++at) {
        const auto &spec = bindings_[at];
        uint32_t binding_index = 0;
        uint32_t executor_index = 0;
        if (!bindings.find(view(spec.binding), spec.binding_version, binding_index) ||
            !find_executor(view(spec.executor), spec.executor_version, executor_index)) {
            return AYNI_NOT_FOUND;
        }
        const auto &selection = bindings.at(binding_index);
        const auto capability =
            capabilities.find(view(selection.capability), selection.capability_version);
        uint32_t call_index = 0;
        if (capability.kind != CapabilityRegistry::Lookup::Kind::found ||
            !calls.find(view(selection.capability), selection.capability_version, call_index)) {
            return AYNI_NOT_FOUND;
        }
        CallContract contract{};
        const auto resolved =
            BindingRegistry::resolve_contract(selection, calls.at(call_index).contract, contract);
        if (resolved != AYNI_OK) {
            return resolved;
        }
        const auto &executor = executors_[executor_index].ports;
        if (contract.form != CallForm::action || contract.category != CallClass::tool ||
            (contract.effects & call_effect_physical) == 0 || !actuator(contract, resources) ||
            spec.lease_us > (spec.form == AYNI_FORM_PROCEDURE ? executor.procedure_max_us
                                                              : executor.session_lease_max_us) ||
            executor.poll_period_us >= spec.lease_us ||
            spec.max_command_age_us > executor.command_age_max_us) {
            return AYNI_INVALID_ARGUMENT;
        }
        if (contract.business_contract.size != 0U) {
            DecodedBusiness proof;
            const auto checked = decode_business(capabilities.domain_view(capability.index),
                                                 contract.business_contract, proof);
            if (checked != AYNI_OK) {
                return checked;
            }
            for (uint32_t field = 0; field < spec.field_count; ++field) {
                const auto &control = spec.fields[field];
                for (uint32_t path = 0; path < proof.count; ++path) {
                    const auto &projection = proof.projections[path];
                    if (projection.field_id != control.minimum_field &&
                        projection.field_id != control.maximum_field) {
                        continue;
                    }
                    for (uint32_t depth = 0; depth < projection.path_count; ++depth) {
                        // ADR-0084: one permit cannot silently select the first
                        // element of a multi-value physical control process.
                        if (projection.path[depth] == AYNI_PROJECTION_LIST_ITEM) {
                            return AYNI_UNSUPPORTED;
                        }
                    }
                }
            }
        }
        std::array<int64_t, AYNI_CONTROL_FIELD_COUNT_MAX> minima{};
        std::array<int64_t, AYNI_CONTROL_FIELD_COUNT_MAX> maxima{};
        bool concrete = false;
        if (!read_projection(spec, capabilities.domain_view(capability.index), minima, maxima,
                             concrete)) {
            return AYNI_INVALID_ARGUMENT;
        }
    }
    return validate_ownership(bindings, calls, resources);
}
// Pairwise bounded startup proof keeps executor exclusivity inside the resource
// admission mechanism. A second binding cannot reserve unrelated resources and
// strand a claim while the same local gate is already occupied (ADR-0050).
ayni_status_t ExecutionRegistry::validate_ownership(const BindingRegistry &bindings,
                                                    const CallCapabilityRegistry &calls,
                                                    const ResourceRegistry &resources) const {
    std::array<uint32_t, AYNI_EXECUTOR_CAPACITY_MAX> owned{};
    owned.fill(UINT32_MAX);
    for (uint32_t at = 0; at < bindings.size(); ++at) {
        const auto &selection = bindings.at(at);
        uint32_t call = 0;
        if (!calls.find(view(selection.capability), selection.capability_version, call)) {
            return AYNI_NOT_FOUND;
        }
        CallContract contract{};
        const auto resolved =
            BindingRegistry::resolve_contract(selection, calls.at(call).contract, contract);
        if (resolved != AYNI_OK) {
            return resolved;
        }
        if ((contract.effects & call_effect_physical) == 0) {
            continue;
        }
        uint32_t mapping = 0;
        uint32_t executor = 0;
        if (!find_binding(view(selection.id), selection.version, mapping) ||
            !find_executor(view(bindings_[mapping].executor), bindings_[mapping].executor_version,
                           executor)) {
            return AYNI_NOT_FOUND;
        }
        uint32_t actuator_index = UINT32_MAX;
        for (uint32_t member = 0; member < contract.resource_count; ++member) {
            uint32_t resource = 0;
            if (!resources.find(view(contract.resources[member].id), resource)) {
                return AYNI_NOT_FOUND;
            }
            if ((resources.at(resource).flags & AYNI_RESOURCE_ACTUATOR) != 0) {
                actuator_index = resource;
                break;
            }
        }
        if (actuator_index == UINT32_MAX ||
            (owned[executor] != UINT32_MAX && owned[executor] != actuator_index)) {
            return AYNI_INVALID_ARGUMENT;
        }
        owned[executor] = actuator_index;
    }
    return AYNI_OK;
}
} // namespace ayni::catalog
