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
    return execution.validate_all(bindings, capabilities, call_capabilities, resources);
}

} // namespace ayni::kernel
