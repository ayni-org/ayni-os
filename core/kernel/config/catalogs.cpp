#include "kernel/config/catalogs.h"

namespace ayni::kernel {

Catalogs::Catalogs(const KernelCapacities &capacities, const KernelStorage &storage)
    : goal_types(storage[Store::goal_types], capacities.goal_type_capacity),
      strategies(storage[Store::strategies], capacities.strategy_capacity),
      capabilities(storage[Store::capabilities], capacities.capability_capacity,
                   capacities.authority_scope_max_bytes),
      observation_sources(storage[Store::observation_sources],
                          capacities.observation_source_capacity),
      execution(storage[Store::execution_catalog], capacities.executor_capacity,
                capacities.execution_binding_capacity),
      resources(storage[Store::resource_catalog], capacities.resource_capacity),
      devices(storage[Store::devices], capacities.device_capacity, storage[Store::components],
              capacities.component_capacity),
      call_capabilities(storage[Store::call_capabilities], capacities.call_capability_capacity),
      providers(storage[Store::providers], capacities.provider_capacity),
      bindings(storage[Store::bindings], capacities.binding_capacity) {}

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
    const auto linked =
        bindings.validate_all(capabilities, call_capabilities, providers, resources);
    if (linked != AYNI_OK) {
        return linked;
    }
    const auto executable =
        execution.validate_all(bindings, capabilities, call_capabilities, resources);
    if (executable != AYNI_OK) {
        return executable;
    }
    // A model may propose only tools a strategy could request (ADR-0054).
    return goal_types.link_tools([&](const catalog::GoalTypeTool &tool, uint32_t &out_index) {
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
        if (call_capabilities.at(links.call).contract.category != catalog::CallClass::tool ||
            dispatchable(bindings.at(index), links) != AYNI_OK) {
            return AYNI_INVALID_ARGUMENT;
        }
        out_index = index;
        return AYNI_OK;
    });
}

ayni_status_t Catalogs::dispatchable(const catalog::Binding &binding,
                                     const catalog::BindingLinks &links) const {
    const auto &contract = call_capabilities.at(links.call).contract;
    if (contract.form == catalog::CallForm::subscription) {
        return AYNI_UNSUPPORTED; // Subscription lifecycle is delivered separately (ADR-0050).
    }
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
