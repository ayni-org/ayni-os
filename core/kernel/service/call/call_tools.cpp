// Required-policy directory projection. A model's proposals are read by the strategy
// from the result body itself (ADR-0096); receipt never authorizes execution (ADR-0085).
#include "foundation/workspace.h"
#include "kernel/service/call/call_runtime.h"

#include "authority/scope/business_request.h"

#include <cstring>

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

} // namespace

bool CallRuntime::offer_tool(uint32_t task_index, uint32_t position, ToolOffer &out) const {
    const auto &task = work_.tasks.at(task_index);
    const auto &policy = work_.goals.at(task.goal_index).tools;
    if (position >= policy.count) {
        return false;
    }
    const auto &reference = policy.tools[position];
    uint32_t binding_index = 0;
    if (!catalogs_.bindings.find(view(reference.binding_id), reference.binding_version,
                                 binding_index)) {
        return false;
    }
    ToolOffer offer{};
    offer.binding = &catalogs_.bindings.at(binding_index);
    if (catalog::BindingRegistry::resolve(*offer.binding, catalogs_.capabilities,
                                          catalogs_.call_capabilities, catalogs_.providers,
                                          catalogs_.resources, offer.links) != AYNI_OK) {
        return false;
    }
    offer.domain = catalogs_.capabilities.domain_view(offer.links.capability);
    // The same requirement match as check_input, which rechecks any request.
    const auto ceiling = work_.goal_authority.at(task.goal_index);
    for (uint32_t at = 0; at < ceiling.count; ++at) {
        const auto &requirement = ceiling.requirements[at];
        if (requirement.capability_id.equals(view(offer.binding->capability)) &&
            requirement.capability_version == offer.binding->capability_version &&
            requirement.source_domain.size == offer.domain.size &&
            std::memcmp(requirement.source_domain.data, offer.domain.data, offer.domain.size) ==
                0 &&
            authority::binding_route_allowed(*offer.binding, requirement.scope)) {
            offer.ceiling = requirement.scope;
            out = offer;
            return true;
        }
    }
    return false;
}

} // namespace ayni::kernel
