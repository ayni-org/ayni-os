#include "kernel/config/catalogs.h"
#include "catalog/capability/business_encoding.h"
#include "foundation/workspace.h"

namespace ayni::kernel {

Catalogs::Catalogs(const KernelCapacities &capacities, const KernelStorage &storage,
                   foundation::Workspace &workspace)
    : goal_types(storage[Store::goal_types], capacities.goal_type_capacity),
      strategies(storage[Store::strategies], capacities.strategy_capacity),
      capabilities(storage[Store::capabilities], capacities.capability_capacity,
                   capacities.authority_scope_max_bytes, capacities.business_contract_max_bytes),
      observation_sources(storage[Store::observation_sources],
                          capacities.observation_source_capacity),
      execution(storage[Store::execution_catalog], capacities.executor_capacity,
                capacities.execution_binding_capacity),
      resources(storage[Store::resource_catalog], capacities.resource_capacity),
      devices(storage[Store::devices], capacities.device_capacity, storage[Store::components],
              capacities.component_capacity),
      call_capabilities(storage[Store::call_capabilities], capacities.call_capability_capacity,
                        capacities.business_contract_max_bytes),
      providers(storage[Store::providers], capacities.provider_capacity,
                capacities.business_contract_max_bytes),
      bindings(storage[Store::bindings], capacities.binding_capacity), workspace_(workspace) {}

// Registration is closed from here on, so each goal type's strategy is
// resolved once and its index stays valid (C-LIFE-003).
ayni_status_t Catalogs::close() {
    if (!goal_types.link_strategies(strategies)) {
        return AYNI_NOT_FOUND;
    }
    const auto inventory = devices.validate();
    if (inventory != AYNI_OK) {
        return inventory;
    }
    const auto topology = resources.validate(devices);
    if (topology != AYNI_OK) {
        return topology;
    }
    const auto business = link_business();
    if (business != AYNI_OK) {
        return business;
    }
    // Subscription source semantics are exact and independent of registration order.
    for (uint32_t at = 0; at < call_capabilities.size(); ++at) {
        const auto &spec = call_capabilities.at(at).contract.subscription;
        if (!spec) {
            continue;
        }
        uint32_t source = 0;
        if (!observation_sources.find({spec->source.data(), spec->source.size()}, source)) {
            return AYNI_NOT_FOUND;
        }
        if (observation_sources.at(source).version != spec->source_version) {
            return AYNI_UNSUPPORTED_VERSION;
        }
    }
    const auto linked =
        bindings.validate_all(capabilities, call_capabilities, providers, resources);
    if (linked != AYNI_OK) {
        return linked;
    }
    const auto executable =
        execution.validate_all(workspace_, bindings, capabilities, call_capabilities, resources);
    if (executable != AYNI_OK) {
        return executable;
    }
    // A model may propose only tools a strategy could request (ADR-0054).
    return goal_types.validate_tools([&](const catalog::ToolReference &tool) {
        uint32_t index = 0;
        catalog::BindingLinks links{};
        if (!bindings.find({tool.binding_id.data(), tool.binding_id.size()}, tool.binding_version,
                           index)) {
            return AYNI_NOT_FOUND;
        }
        if (catalog::BindingRegistry::resolve(bindings.at(index), capabilities, call_capabilities,
                                              providers, resources, links) != AYNI_OK) {
            return AYNI_INTERNAL; // validate_all resolved every Binding.
        }
        if (links.contract.category != catalog::CallClass::tool ||
            dispatchable(bindings.at(index), links) != AYNI_OK) {
            return AYNI_INVALID_ARGUMENT;
        }
        return AYNI_OK;
    });
}

// Pair original business proofs after all independent declarations are present,
// before recovery may consume an old Grant or Goal under this catalog.
ayni_status_t Catalogs::link_business() {
    // Definitions were copied without registration-order coupling. Close the
    // complete authority/schema relationship before recovery may consume it.
    for (uint32_t at = 0; at < call_capabilities.size(); ++at) {
        const auto &capability = call_capabilities.at(at);
        if (capability.contract.business_contract.size == 0U) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto authority =
            capabilities.find({capability.id.data(), capability.id.size()}, capability.version);
        if (authority.kind != catalog::CapabilityRegistry::Lookup::Kind::found) {
            return AYNI_NOT_FOUND;
        }
        auto business_lease = workspace_.make<catalog::DecodedBusiness>();
        if (!business_lease) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        auto &business = *business_lease;
        const auto checked =
            catalog::decode_business(workspace_, capabilities.domain_view(authority.index),
                                     capability.contract.business_contract, business);
        if (checked != AYNI_OK) {
            return checked;
        }
        if (!catalog::same_call_contract(capability.contract, business.execution)) {
            return AYNI_INVALID_ARGUMENT;
        }
        const auto attached =
            capabilities.attach_business(authority.index, capability.contract.business_contract);
        if (attached != AYNI_OK) {
            return attached;
        }
    }
    return AYNI_OK;
}

ayni_status_t Catalogs::dispatchable(const catalog::Binding &binding,
                                     const catalog::BindingLinks &links) const {
    const auto &contract = links.contract;
    const bool physical = (contract.effects & catalog::call_effect_physical) != 0U;
    uint32_t execution_index = 0;
    if (physical && !execution.find_binding({binding.id.data(), binding.id.size()}, binding.version,
                                            execution_index)) {
        return AYNI_UNSUPPORTED;
    }
    for (uint32_t at = 0; at < contract.resource_count; ++at) {
        // Every actuator acquisition uses the same permit/stop consumer.
        if (!physical && (resources.at(links.resources[at]).flags & AYNI_RESOURCE_ACTUATOR) != 0U) {
            return AYNI_UNSUPPORTED;
        }
    }
    return AYNI_OK;
}

} // namespace ayni::kernel
