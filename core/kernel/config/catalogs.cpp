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
    // A model may propose only ordinary unary/action tools (ADR-0054).
    const auto tools = goal_types.link_tools([&](const catalog::GoalTypeTool &tool,
                                                 uint32_t &out_index) {
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
        const auto &contract = call_capabilities.at(links.call).contract;
        if (contract.category != catalog::CallClass::tool ||
            contract.form == catalog::CallForm::subscription) {
            return AYNI_INVALID_ARGUMENT;
        }
        out_index = index;
        return AYNI_OK;
    });
    if (tools != AYNI_OK) {
        return tools;
    }
    return execution.validate_all(bindings, capabilities, call_capabilities, resources);
}

} // namespace ayni::kernel
