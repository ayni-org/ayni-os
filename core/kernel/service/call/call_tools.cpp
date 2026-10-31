// Required-policy directory projection and Task-owned native model proposals.
// Model receipt never normalizes or authorizes business execution (ADR-0085).
#include "foundation/workspace.h"
#include "kernel/service/call/call_runtime.h"

#include "authority/scope/business_request.h"
#include "catalog/abi_checks.h"
#include "catalog/capability/argument_encoding.h"
#include "kernel/service/memory/memory_access.h"
#include "work/call/tool_calls.h"

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

// Task-scoped like read_result: only this Task's own retained calls, during decide.
ayni_status_t CallRuntime::read_tool_call(const void *context, ayni_string_v1 step, uint32_t index,
                                          ayni_task_tool_call_v1 *out) {
    if (context == nullptr || out == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto shape = catalog::check_struct_size(out->struct_size, sizeof(*out), sizeof(*out));
    if (shape != AYNI_OK) {
        return shape;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    if (!runtime.decision_ || !runtime.work_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    if (!task_content_current({runtime.work_, runtime.host_.monotonic_now_us(runtime.host_.ctx),
                               runtime.decision_->durable_seq},
                              runtime.decision_->task_index)) {
        return AYNI_EXPIRED;
    }
    const auto &task = runtime.work_.tasks.at(runtime.decision_->task_index);
    uint32_t call_index = 0;
    work::ToolCallView call{};
    if (!runtime.work_.calls.find_step(task.task_id, step, call_index) ||
        !work::read_tool_call(runtime.work_.workspace, runtime.work_.calls.tool_calls(call_index),
                              index, call)) {
        return AYNI_NOT_FOUND;
    }
    if (!runtime.retain_model_context(
            runtime.decision_->task_index, runtime.work_.calls.identity(call_index).model_context,
            runtime.host_.monotonic_now_us(runtime.host_.ctx), runtime.decision_->durable_seq)) {
        return AYNI_EXPIRED;
    }
    out->reserved = 0;
    out->call = {}; // The caller may reuse storage from another proposal kind.
    out->call.struct_size = sizeof(out->call);
    out->call.function_name = call.function_name;
    out->call.protocol_id = call.protocol_id;
    out->call.input_kind = call.input_kind;
    if (call.input_kind == AYNI_TOOL_INPUT_ARGUMENTS) {
        auto arguments_lease = runtime.work_.workspace.make<catalog::DecodedArguments>();
        if (!arguments_lease) {
            return AYNI_CAPACITY_EXCEEDED;
        }
        auto &arguments = *arguments_lease;
        const auto status =
            catalog::decode_arguments(runtime.work_.workspace, call.input, arguments);
        if (status != AYNI_OK) {
            return status;
        }
        for (uint32_t at = 0; at < arguments.count; ++at) {
            out->nodes[at] = arguments.nodes[at];
        }
        out->call.input.arguments = {out->nodes, arguments.count};
    } else {
        out->call.input.invalid_arguments = {reinterpret_cast<const char *>(call.input.data),
                                             call.input.size};
    }
    return AYNI_OK;
}
} // namespace ayni::kernel
