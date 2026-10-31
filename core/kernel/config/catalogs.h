// The registries the Host fills in CONFIGURING (ADR-0051): what kinds of
// goals, strategies, authorization schemas, observation sources, executors,
// resources, devices, call capabilities, Providers and bindings exist. They
// are trusted, immutable once ayni_start closes them, and hold no live
// state. ayni_kernel owns them; every other owner borrows them as const
// Catalogs&, so a write after start does not compile.

#ifndef AYNI_CORE_KERNEL_CONFIG_CATALOGS_H
#define AYNI_CORE_KERNEL_CONFIG_CATALOGS_H

#include "catalog/binding/binding_registry.h"
#include "catalog/capability/capability_registry.h"
#include "catalog/device/device_catalog.h"
#include "catalog/execution/execution_registry.h"
#include "catalog/goal_type/goal_type_registry.h"
#include "catalog/observation/source_registry.h"
#include "catalog/provider/provider_registry.h"
#include "catalog/resource/resource_registry.h"
#include "catalog/strategy/strategy_registry.h"
#include "kernel/config/kernel_config.h"

#include <ayni/ayni.h>

namespace ayni::kernel {

struct Catalogs {
    // Each registry over its store in storage, sized by capacities.
    Catalogs(const KernelCapacities &capacities, const KernelStorage &storage,
             foundation::Workspace &workspace);
    Catalogs(const Catalogs &) = delete;
    Catalogs &operator=(const Catalogs &) = delete;

    // Ends registration (ayni_start): links each goal type to its strategy,
    // then checks the device inventory, the resource topology, every
    // binding and every execution binding against the catalogs they name,
    // then links each goal type's tools to dispatchable TOOL Bindings, in
    // that order. The first failure's status, or AYNI_OK.
    ayni_status_t close();
    // Whether a resolved Binding's calls can run in this build: AYNI_OK, or
    // AYNI_UNSUPPORTED for a subscription, a physical Binding without an
    // ExecutionBinding, or a nonphysical one holding an actuator (ADR-0050).
    ayni_status_t dispatchable(const catalog::Binding &binding,
                               const catalog::BindingLinks &links) const;

    catalog::GoalTypeRegistry goal_types;
    catalog::StrategyRegistry strategies;
    catalog::CapabilityRegistry capabilities; // Authorization schemas; registration enables no I/O.
    catalog::ObservationSources observation_sources; // Shared by Scenes and knowledge.
    catalog::ExecutionRegistry execution;            // Executor ports and limits, not live permits.
    catalog::ResourceRegistry resources;             // Resource ids and capacities, not occupancy.
    catalog::DeviceCatalog devices;                  // Inventory, not health.
    catalog::CallCapabilityRegistry call_capabilities; // Execution semantics, not authority.
    catalog::ProviderRegistry providers; // Callback descriptors whose contexts the runtime owns.
    catalog::BindingRegistry bindings;   // Exact selections, with no automatic fallback.
  private:
    // Completes authority/business-proof pairing only during configuration close.
    ayni_status_t link_business();
    foundation::Workspace &workspace_; // Borrowed Kernel temporary owner, not catalog state.
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_CONFIG_CATALOGS_H
