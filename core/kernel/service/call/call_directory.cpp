// Directory visibility is a current authorization projection, never a Call
// admission or a backend query. All metadata was adopted before start.
#include "authority/scope/business_request.h"
#include "catalog/abi_checks.h"
#include "kernel/service/call/call_runtime.h"

namespace ayni::kernel {
namespace {
ayni_string_v1 view(const catalog::CatalogId &id) {
    return {id.data(), id.size()};
}
bool first_page(const ayni_capability_cursor_v1 &cursor) {
    return !cursor.boot_epoch && !cursor.task_id && !cursor.commit_seq && !cursor.durable_seq &&
           !cursor.valid_until_us && !cursor.gate_count && !cursor.position;
}
ayni_capability_info_v1 info(const catalog::BindingRegistry &bindings, uint32_t index,
                             ayni_tool_role_t role) {
    const auto &binding = bindings.at(index);
    return {view(binding.id),
            binding.version,
            view(binding.capability),
            binding.capability_version,
            role,
            bindings.summary(index)};
}
} // namespace

// Listing checks policy and a live complete Grant chain. It does not assert
// that arbitrary arguments fit that Grant or the ancestors' saved ceilings.
bool CallRuntime::visible_capability(uint32_t task_index, uint32_t position, uint64_t now_us,
                                     uint64_t durable_seq, uint32_t &binding_index,
                                     catalog::BindingLinks &links) const {
    const auto &task = work_.tasks.at(task_index);
    const auto &policy = work_.goals.at(task.goal_index).tools;
    if (position >= policy.count) {
        return false;
    }
    const auto &reference = policy.tools[position];
    if (!catalogs_.bindings.find(view(reference.binding_id), reference.binding_version,
                                 binding_index) ||
        !catalogs_.bindings.has_description(binding_index)) {
        return false;
    }
    const auto &binding = catalogs_.bindings.at(binding_index);
    if (!work_.tool_policy_allows(task.goal_index, binding) ||
        catalog::BindingRegistry::resolve(binding, catalogs_.capabilities,
                                          catalogs_.call_capabilities, catalogs_.providers,
                                          catalogs_.resources, links) != AYNI_OK) {
        return false;
    }
    const auto &contract = catalogs_.call_capabilities.at(links.call).contract;
    if (contract.category != catalog::CallClass::tool) {
        return false;
    }
    if (!on_demand_tool(task_index, binding, contract)) {
        ToolOffer offer{};
        return offer_tool(task_index, position, offer);
    }
    const auto domain = catalogs_.capabilities.domain_view(links.capability);
    for (uint32_t at = 0; at < work_.grants.size(); ++at) {
        const auto &grant = work_.grants.at(at);
        if (grant.subject_instance_id == task.owner_instance_id &&
            work_.grants.covers_saved_scope(at, binding.capability, binding.capability_version,
                                            domain, work_.grants.scope(at),
                                            contract.business_contract) &&
            authority::binding_route_allowed(binding, work_.grants.scope(at)) &&
            work_.check_grant(at, now_us, durable_seq, task.goal_id).status == AYNI_OK) {
            return true;
        }
    }
    return false;
}

// Conservative invalidation includes all work changes, durability advances,
// pending deny gates and the earliest finite Grant expiry. Cursor positions
// can never bypass per-entry checks, even when supplied by an untrusted caller.
ayni_status_t CallRuntime::list_capabilities(uint32_t task_index, uint64_t now_us,
                                             uint64_t durable_seq,
                                             const ayni_capability_cursor_v1 &cursor,
                                             ayni_capability_info_v1 *items, uint32_t capacity,
                                             uint32_t &count,
                                             ayni_capability_cursor_v1 &next) const {
    if (!items || !capacity || capacity > AYNI_CAPABILITY_PAGE_MAX) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &task = work_.tasks.at(task_index);
    if (!work::TaskTable::is_live(task.lifecycle)) {
        return AYNI_NOT_FOUND;
    }
    // Task refusal precedes cursor diagnostics; no saved page is an authority lease.
    const auto authority = work_.check_task_authority(task_index, now_us, durable_seq);
    if (authority.status != AYNI_OK) {
        return authority.status;
    }
    const auto &policy = work_.goals.at(task.goal_index).tools;
    const auto commit_seq = work_.transitions.next_commit_seq() - 1U;
    const auto boot = work_.ids.boot_epoch();
    const bool first = first_page(cursor);
    if (!first && (cursor.boot_epoch != boot || cursor.task_id != task.task_id ||
                   cursor.commit_seq != commit_seq || cursor.durable_seq != durable_seq ||
                   cursor.gate_count != work_.grants.gate_count() ||
                   cursor.position >= policy.count || now_us >= cursor.valid_until_us)) {
        return AYNI_REVISION_CONFLICT;
    }
    uint64_t valid_until = UINT64_MAX;
    for (uint32_t at = 0; at < work_.grants.size(); ++at) {
        const auto &grant = work_.grants.at(at);
        if (grant.lifecycle == AYNI_GRANT_ACTIVE && grant.deadline_us > now_us &&
            grant.deadline_us < valid_until) {
            valid_until = grant.deadline_us;
        }
    }
    std::array<ayni_capability_info_v1, AYNI_CAPABILITY_PAGE_MAX> page{};
    uint32_t used = 0;
    uint32_t position = first ? 0U : cursor.position;
    for (; position < policy.count; ++position) {
        uint32_t binding = 0;
        catalog::BindingLinks links{};
        if (!visible_capability(task_index, position, now_us, durable_seq, binding, links)) {
            continue;
        }
        if (used == capacity) {
            break;
        }
        page[used++] = info(catalogs_.bindings, binding, policy.tools[position].role);
    }
    const ayni_capability_cursor_v1 following =
        position == policy.count
            ? ayni_capability_cursor_v1{}
            : ayni_capability_cursor_v1{boot,        task.task_id, commit_seq,
                                        durable_seq, valid_until,  work_.grants.gate_count(),
                                        position};
    for (uint32_t at = 0; at < used; ++at) {
        items[at] = page[at];
    }
    count = used;
    next = following;
    return AYNI_OK;
}

// Unknown, unlisted, missing-metadata and currently unauthorized selections
// share NOT_FOUND. No lookup callback or cached directory lease grants access.
ayni_status_t CallRuntime::inspect_capability(uint32_t task_index, uint64_t now_us,
                                              uint64_t durable_seq, ayni_string_v1 binding_id,
                                              uint32_t version,
                                              ayni_capability_detail_v1 &out) const {
    if (const auto shape = catalog::check_struct_size(out.struct_size, sizeof(out), sizeof(out));
        shape != AYNI_OK) {
        return shape;
    }
    if (out.reserved || !version || !catalog::CatalogId::is_valid(binding_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &task = work_.tasks.at(task_index);
    if (!work::TaskTable::is_live(task.lifecycle)) {
        return AYNI_NOT_FOUND;
    }
    const auto authority = work_.check_task_authority(task_index, now_us, durable_seq);
    if (authority.status != AYNI_OK) {
        return authority.status;
    }
    const auto &policy = work_.goals.at(task.goal_index).tools;
    for (uint32_t at = 0; at < policy.count; ++at) {
        const auto &reference = policy.tools[at];
        if (reference.binding_version != version || !reference.binding_id.equals(binding_id)) {
            continue;
        }
        uint32_t binding = 0;
        catalog::BindingLinks links{};
        if (!visible_capability(task_index, at, now_us, durable_seq, binding, links)) {
            break;
        }
        std::array<ayni_tool_field_usage_v1, AYNI_AUTHORITY_FIELD_COUNT_MAX> fields{};
        ayni_tool_usage_v1 usage{};
        catalogs_.bindings.usage(binding, usage, fields);
        out = {};
        out.struct_size = sizeof(out);
        out.info = info(catalogs_.bindings, binding, reference.role);
        out.original_domain = catalogs_.capabilities.domain_view(links.capability);
        out.business_contract =
            catalogs_.call_capabilities.at(links.call).contract.business_contract;
        out.usage = usage;
        for (uint32_t field = 0; field < usage.field_count; ++field) {
            out.fields[field] = fields[field];
        }
        out.usage.fields = usage.field_count ? out.fields : nullptr;
        return AYNI_OK;
    }
    return AYNI_NOT_FOUND;
}

ayni_status_t CallRuntime::list_capabilities_view(const void *context,
                                                  const ayni_capability_cursor_v1 *cursor,
                                                  ayni_capability_info_v1 *items, uint32_t capacity,
                                                  uint32_t *count,
                                                  ayni_capability_cursor_v1 *next) {
    if (!context || !cursor || !count || !next) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    if (!runtime.decision_ || !runtime.work_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    return runtime.list_capabilities(
        runtime.decision_->task_index, runtime.host_.monotonic_now_us(runtime.host_.ctx),
        runtime.decision_->durable_seq, *cursor, items, capacity, *count, *next);
}
ayni_status_t CallRuntime::inspect_capability_view(const void *context, ayni_string_v1 binding_id,
                                                   uint32_t version,
                                                   ayni_capability_detail_v1 *out) {
    if (!context || !out) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto &runtime = *static_cast<const CallRuntime *>(context);
    if (!runtime.decision_ || !runtime.work_.calling_out()) {
        return AYNI_INVALID_STATE;
    }
    return runtime.inspect_capability(runtime.decision_->task_index,
                                      runtime.host_.monotonic_now_us(runtime.host_.ctx),
                                      runtime.decision_->durable_seq, binding_id, version, *out);
}
} // namespace ayni::kernel
