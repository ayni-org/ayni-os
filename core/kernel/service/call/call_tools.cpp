// Tools a model call may propose (ADR-0054): the GoalType's allow list, cut to
// what the Task's Goal is authorized for, described to the Provider at submit
// and checked again on receipt. Requesting a proposed call is the strategy's
// separate decision, checked like any other request.
#include "kernel/service/call/call_runtime.h"

#include "authority/scope/scope.h"
#include "work/call/tool_calls.h"

#include <cstring>

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}

// A target or egress field reaches only the Binding's own endpoint or
// destination; ordinary parameters have no route.
bool routed(ayni_authority_field_usage_t usage) {
    return usage == AYNI_AUTHORITY_EXECUTION_TARGET || usage == AYNI_AUTHORITY_EGRESS_DESTINATION;
}

ayni_string_v1 route(const catalog::Binding &binding, ayni_authority_field_usage_t usage) {
    return view(usage == AYNI_AUTHORITY_EXECUTION_TARGET ? binding.endpoint : binding.destination);
}

bool same(ayni_string_v1 left, ayni_string_v1 right) {
    return catalog::compare_tokens(left, right) == 0;
}

// Whether a ceiling lets this Binding's route through at all.
bool admits_route(const catalog::Binding &binding, ayni_bytes_v1 ceiling) {
    catalog::DomainReader reader(ceiling);
    catalog::DomainField field;
    while (reader.next(field)) {
        if (!routed(field.usage)) {
            continue;
        }
        bool found = false;
        for (uint32_t at = 0; at < field.values.token_count; ++at) {
            found = found || same(field.values.tokens[at], route(binding, field.usage));
        }
        if (!found) {
            return false;
        }
    }
    return reader.ok();
}

// Whether a decoded call's routed fields name this Binding's route.
bool routes_to(const catalog::Binding &binding, ayni_bytes_v1 domain,
               const work::ToolCallView &call) {
    catalog::DomainReader reader(domain);
    catalog::DomainField field;
    while (reader.next(field)) {
        if (!routed(field.usage)) {
            continue;
        }
        bool found = false;
        for (uint32_t at = 0; at < call.field_count; ++at) {
            found = found || (call.fields[at].id == field.id &&
                              same(call.fields[at].token, route(binding, field.usage)));
        }
        if (!found) {
            return false;
        }
    }
    return reader.ok();
}

// Position of the index-th set bit; false when there are not that many.
bool nth_position(uint32_t positions, uint32_t index, uint32_t &out) {
    for (uint32_t position = 0; position < AYNI_GOAL_TYPE_TOOL_COUNT_MAX; ++position) {
        if ((positions & (1U << position)) == 0U) {
            continue;
        }
        if (index == 0U) {
            out = position;
            return true;
        }
        --index;
    }
    return false;
}
} // namespace

bool CallRuntime::offer_tool(uint32_t task_index, uint32_t position, ToolOffer &out) const {
    const auto &task = work_.tasks.at(task_index);
    const auto type_index = work_.goals.at(task.goal_index).goal_type_index;
    if (type_index == work::GoalTable::no_index) {
        return false;
    }
    const auto &type = catalogs_.goal_types.at(type_index);
    if (position >= type.tool_count) {
        return false;
    }
    ToolOffer offer{};
    offer.binding = &catalogs_.bindings.at(type.tools[position].binding_index);
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
            admits_route(*offer.binding, requirement.scope)) {
            offer.ceiling = requirement.scope;
            out = offer;
            return true;
        }
    }
    return false;
}

uint32_t CallRuntime::offered_tools(uint32_t task_index) const {
    uint32_t positions = 0;
    ToolOffer offer{};
    for (uint32_t position = 0; position < AYNI_GOAL_TYPE_TOOL_COUNT_MAX; ++position) {
        if (offer_tool(task_index, position, offer)) {
            positions |= 1U << position;
        }
    }
    return positions;
}

// Fills the Provider's storage in place, since token sets point into it.
ayni_status_t CallRuntime::describe_tool(const void *context, uint32_t index, ayni_tool_v1 *out) {
    if (context == nullptr || out == nullptr || out->struct_size != sizeof(*out)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    if (!runtime.offering_) {
        return AYNI_INVALID_STATE;
    }
    uint32_t position = 0;
    ToolOffer offer{};
    if (!nth_position(runtime.offering_->positions, index, position)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!runtime.offer_tool(runtime.offering_->task_index, position, offer)) {
        return AYNI_INTERNAL; // Immutable inputs offered it when submit began.
    }
    const auto &binding = *offer.binding;
    *out = {};
    out->struct_size = sizeof(*out);
    out->binding_version = binding.version;
    out->binding_id = view(binding.id);
    out->capability_id = view(binding.capability);
    out->capability_version = binding.capability_version;
    out->text_limit =
        runtime.catalogs_.call_capabilities.at(offer.links.call).contract.input_text_limit;
    catalog::DomainReader reader(offer.ceiling);
    catalog::DomainField field;
    while (out->field_count < AYNI_AUTHORITY_FIELD_COUNT_MAX && reader.next(field)) {
        auto &desc = out->fields[out->field_count];
        auto *tokens = out->tokens[out->field_count];
        desc.struct_size = sizeof(desc);
        desc.field_id = field.id;
        desc.usage = field.usage;
        desc.domain.struct_size = sizeof(desc.domain);
        desc.domain.kind = field.values.kind;
        desc.domain.unit = field.values.unit;
        if (field.values.kind == AYNI_AUTHORITY_I64) {
            desc.domain.values.integer = {field.values.minimum, field.values.maximum};
        } else if (routed(field.usage)) {
            tokens[0] = route(binding, field.usage);
            desc.domain.values.tokens = {tokens, 1, 0};
        } else {
            for (uint32_t at = 0; at < field.values.token_count; ++at) {
                tokens[at] = field.values.tokens[at];
            }
            desc.domain.values.tokens = {tokens, field.values.token_count, 0};
        }
        ++out->field_count;
    }
    return reader.ok() ? AYNI_OK : AYNI_INTERNAL;
}

bool CallRuntime::valid_tool_calls(uint32_t task_index, uint32_t positions,
                                   ayni_bytes_v1 list) const {
    const uint32_t count = work::tool_call_count(list);
    const auto type_index = work_.goals.at(work_.tasks.at(task_index).goal_index).goal_type_index;
    if (count == 0U || positions == 0U || type_index == work::GoalTable::no_index) {
        return false;
    }
    const auto &type = catalogs_.goal_types.at(type_index);
    for (uint32_t at = 0; at < count; ++at) {
        work::ToolCallView call{};
        if (!work::read_tool_call(list, at, call)) {
            return false;
        }
        uint32_t position = 0;
        while (position < type.tool_count &&
               ((positions & (1U << position)) == 0U ||
                !type.tools[position].binding_id.equals(call.binding_id) ||
                type.tools[position].binding_version != call.binding_version)) {
            ++position;
        }
        ToolOffer offer{};
        if (position == type.tool_count || !offer_tool(task_index, position, offer)) {
            return false;
        }
        std::array<ayni_scope_field_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> fields{};
        std::array<ayni_string_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> tokens{};
        const auto covered = authority::contains_request(offer.domain, offer.ceiling,
                                                         call.scope(fields.data(), tokens.data()));
        if (covered.status != AYNI_OK || !covered.covered ||
            !routes_to(*offer.binding, offer.domain, call) ||
            work::check_call_text(call.text, catalogs_.call_capabilities.at(offer.links.call)
                                                 .contract.input_text_limit) != AYNI_OK) {
            return false;
        }
    }
    return true;
}

// Task-scoped like read_result: only this Task's own retained calls, during decide.
ayni_status_t CallRuntime::read_tool_call(const void *context, ayni_string_v1 step, uint32_t index,
                                          ayni_task_tool_call_v1 *out) {
    if (context == nullptr || out == nullptr || out->struct_size != sizeof(*out)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    if (!runtime.decision_ || !runtime.work_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    const auto &task = runtime.work_.tasks.at(runtime.decision_->task_index);
    uint32_t call_index = 0;
    work::ToolCallView call{};
    if (!runtime.work_.calls.find_step(task.task_id, step, call_index) ||
        !work::read_tool_call(runtime.work_.calls.tool_calls(call_index), index, call)) {
        return AYNI_NOT_FOUND;
    }
    out->reserved = 0;
    out->call.struct_size = sizeof(out->call);
    out->call.binding_version = call.binding_version;
    out->call.binding_id = call.binding_id;
    out->call.arguments = call.scope(out->fields, out->tokens);
    out->call.text = call.text;
    return AYNI_OK;
}
} // namespace ayni::kernel
