// Composition boundary for already gated ABI operations (ADR-0058). Read-only
// views never grant mutation; each change invokes the module that owns its fact.
#include "catalog/capability/field_domain.h"
#include "foundation/workspace.h"
#include "kernel/runtime/kernel_state.h"

namespace {
// Registering an id twice is an argument error; the capacity comes last.
template <typename Result> ayni_status_t add_status(Result result) {
    switch (result) {
    case Result::added:
        return AYNI_OK;
    case Result::duplicate:
        return AYNI_INVALID_ARGUMENT;
    case Result::full:
        return AYNI_CAPACITY_EXCEEDED;
    }
    return AYNI_INTERNAL;
}
} // namespace

void ayni_kernel::describe(ayni_kernel_info_v1 &out) const {
    out = {};
    out.struct_size = sizeof(out);
    out.lifecycle = lifecycle;
    out.restriction = restriction();
    out.boot_epoch = boot.boot_epoch();
    out.durable_seq = durable_seq();
    out.events_unmatched = events_unmatched;
    out.events_discarded = events_discarded;
}

ayni_status_t ayni_kernel::configure(const ayni::authority::Principal &principal) {
    return work.principals.add_owner(principal) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}
ayni_status_t ayni_kernel::configure_agent(uint64_t agent_id) {
    return add_status(work.agents.add(agent_id));
}
ayni_status_t ayni_kernel::configure(const ayni::catalog::GoalType &type) {
    return add_status(catalogs.goal_types.add(type));
}
ayni_status_t ayni_kernel::configure(const ayni::catalog::Strategy &strategy) {
    return add_status(catalogs.strategies.add(strategy));
}
ayni_status_t ayni_kernel::configure(const ayni_capability_desc_v1 &capability,
                                     const ayni::catalog::DomainEncoding &encoding) {
    return catalogs.capabilities.add_validated(capability, encoding);
}
ayni_status_t ayni_kernel::configure(const ayni::catalog::Device &device) {
    return catalogs.devices.add_validated(device);
}
ayni_status_t ayni_kernel::configure(const ayni::catalog::Component &component) {
    return catalogs.devices.add_validated(component);
}
ayni_status_t ayni_kernel::configure(const ayni::catalog::Resource &resource) {
    return catalogs.resources.add_validated(resource);
}
ayni_status_t ayni_kernel::configure(const ayni::catalog::ObservationSource &source) {
    return catalogs.observation_sources.add_validated(source);
}
ayni_status_t ayni_kernel::configure(const ayni::catalog::CallCapability &capability,
                                     const ayni_business_contract_v1 *definition) {
    return catalogs.call_capabilities.add_validated(workspace, capability, definition);
}
ayni_status_t
ayni_kernel::configure(const ayni::catalog::Provider &provider,
                       const ayni::catalog::ProviderBusinessDefinitions &definitions) {
    return catalogs.providers.add_validated(workspace, provider, definitions);
}
ayni_status_t ayni_kernel::configure(const ayni::catalog::Binding &binding) {
    return catalogs.bindings.add_validated(binding);
}
// Exact declarations must already exist. Metadata cannot resolve or replace an
// implementation, and registration never executes a backend or grants authority.
ayni_status_t ayni_kernel::configure_usage(ayni_string_v1 summary,
                                           const ayni_tool_usage_v1 &usage) {
    uint32_t index = 0;
    if (!catalogs.bindings.find(usage.binding_id, usage.binding_version, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &binding = catalogs.bindings.at(index);
    const auto capability = catalogs.capabilities.find(
        {binding.capability.data(), binding.capability.size()}, binding.capability_version);
    if (capability.kind != ayni::catalog::CapabilityRegistry::Lookup::Kind::found) {
        return AYNI_NOT_FOUND;
    }
    // Field identity belongs to the authoritative domain, not explanatory input.
    ayni_tool_v1 tool{};
    tool.struct_size = sizeof(tool);
    tool.binding_id = {binding.id.data(), binding.id.size()};
    tool.binding_version = binding.version;
    tool.capability_id = {binding.capability.data(), binding.capability.size()};
    tool.capability_version = binding.capability_version;
    ayni::catalog::DomainReader reader(catalogs.capabilities.domain_view(capability.index));
    ayni::catalog::DomainField field;
    while (reader.next(field)) {
        tool.fields[tool.field_count++].field_id = field.id;
    }
    if (!reader.ok()) {
        return AYNI_INTERNAL;
    }
    return catalogs.bindings.describe(index, summary, usage, tool);
}
ayni_status_t ayni_kernel::configure(const ayni_executor_v1 &executor) {
    return catalogs.execution.add_executor(executor);
}
ayni_status_t ayni_kernel::configure(const ayni_execution_binding_v1 &binding) {
    return catalogs.execution.add_binding(binding);
}
ayni_status_t ayni_kernel::configure(const ayni_scene_desc_v1 &scene) {
    return work.scenes.register_description(scene);
}
// Reflex rules are volatile RAM overlays (ADR-0112): enabling wakes the owner so an
// already-held observation can fire on the next step without waiting for a new sensor write.
ayni_status_t ayni_kernel::configure(const ayni_reflex_rule_v1 &rule) {
    return reflex_table.add_rule(rule);
}
ayni_status_t ayni_kernel::set_reflex_enabled(ayni_string_v1 rule_id, uint32_t enabled) {
    const auto status = reflex_table.set_enabled(rule_id, enabled);
    if (status == AYNI_OK && enabled != 0U) {
        wake();
    }
    return status;
}
ayni_status_t ayni_kernel::get_reflex_info(ayni_string_v1 rule_id, ayni_reflex_info_v1 &out) const {
    return reflex_table.info(rule_id, out);
}
ayni_status_t ayni_kernel::take_reflex_event(ayni_reflex_event_v1 &out) {
    return reflex_table.take_event(out);
}
ayni_status_t ayni_kernel::open_session(const ayni::authority::Principal &principal,
                                        ayni_channel_t channel, uint64_t &out_session_id) {
    return ayni::kernel::open_session(work, principal, channel, out_session_id);
}
ayni_status_t ayni_kernel::close_session(uint64_t session_id) {
    return work.sessions.close(session_id) ? AYNI_OK : AYNI_NOT_FOUND;
}
ayni_status_t ayni_kernel::set_resource_health(uint32_t index, ayni_resource_health_t health) {
    bool changed = false;
    const auto status = resources.set_health(index, health, changed);
    if (status == AYNI_OK && changed) {
        wake();
    }
    return status;
}
ayni_status_t
ayni_kernel::publish_observation(const ayni::knowledge::ObservationInput &observation) {
    const auto status = observations.publish(observation, now_us());
    if (status == AYNI_OK) {
        wake();
    }
    return status;
}
// Ingress owns its lock and admission gate. Never inspect owner-thread lifecycle
// or CallbackGate from an arbitrary producer thread (C-HOST-001, ADR-0021).
ayni_status_t ayni_kernel::post_local_input(const ayni::catalog::CatalogId &source) {
    const auto status = ingress.post_local_input(source);
    if (status == AYNI_OK) {
        wake();
    }
    return status;
}

ayni_status_t ayni_kernel::post_observation(uint64_t boot_epoch,
                                            const ayni::knowledge::ObservationInput &input) {
    const auto status = observation_ingress.post(boot_epoch, input);
    if (status == AYNI_OK) {
        wake();
    }
    return status;
}
// Lock-free latest-wins slot write (ADR-0112): wakes the owner only on clean->dirty
// transitions so high-rate sensor threads do not flood the Host wakeup signal.
ayni_status_t
ayni_kernel::publish_observation_latest(uint64_t boot_epoch,
                                        const ayni::knowledge::ObservationInput &input) {
    bool wake_owner = false;
    const auto status = observation_slots.publish_latest(boot_epoch, input, wake_owner);
    if (status == AYNI_OK && wake_owner) {
        wake();
    }
    return status;
}
ayni_status_t ayni_kernel::report_observation_gap(uint64_t boot_epoch,
                                                  const ayni::catalog::CatalogId &source,
                                                  uint32_t version,
                                                  ayni::knowledge::ObservationBoundary boundary) {
    const auto status = observation_ingress.gap(boot_epoch, source, version, boundary);
    if (status == AYNI_OK) {
        wake();
    }
    return status;
}
