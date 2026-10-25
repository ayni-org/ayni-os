#include "catalog/capability/call_contract.h"

#include "catalog/abi_checks.h"
#include "catalog/capability/field_values.h"

namespace ayni::catalog {
namespace {
ayni_string_v1 view(const CatalogId &id) {
    return {id.data(), id.size()};
}

bool known_form(CallForm form) {
    return form == CallForm::unary || form == CallForm::action || form == CallForm::subscription;
}

bool known_class(CallClass category) {
    return category == CallClass::tool || category == CallClass::model;
}

bool known_idempotency(CallIdempotency value) {
    return value == CallIdempotency::idempotent || value == CallIdempotency::not_idempotent;
}

bool known_query(QueryAuthority query) {
    return query == QueryAuthority::none || query == QueryAuthority::best_effort ||
           query == QueryAuthority::authoritative;
}

bool known_cancel(CancelMode cancel) {
    return cancel == CancelMode::none || cancel == CancelMode::request ||
           cancel == CancelMode::confirmed;
}

// Only boundary shape is handled here. normalize_call_contract remains the
// single owner of enum, limit, resource uniqueness and canonical-order rules.
ayni_status_t decode_contract(const ayni_call_contract_v1 &input, CallContract &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    if (input.resource_count > resource_group_max ||
        (input.resource_count != 0U && input.resources == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    CallContract value{};
    value.form = static_cast<CallForm>(input.form);
    value.category = static_cast<CallClass>(input.category);
    value.effects = input.effects;
    value.idempotency = static_cast<CallIdempotency>(input.idempotency);
    value.query = static_cast<QueryAuthority>(input.query);
    value.cancel = static_cast<CancelMode>(input.cancel);
    value.dedupe_window_ms = input.dedupe_window_ms;
    value.timeout_ms = input.timeout_ms;
    value.input_format = input.input_format;
    value.result_format = input.result_format;
    value.input_text_limit = input.input_text_limit;
    value.result_text_limit = input.result_text_limit;
    value.resource_count = input.resource_count;
    for (uint32_t at = 0; at < value.resource_count; ++at) {
        const auto &resource = input.resources[at];
        if (resource.reserved != 0U || !value.resources[at].id.assign(resource.resource_id)) {
            return AYNI_INVALID_ARGUMENT;
        }
        value.resources[at].units = resource.units;
    }
    return normalize_call_contract(value, out);
}

// Canonical sorted requirement groups have one meaning regardless of registration
// array order. Duplicate resources are invalid, not silently summed or deduplicated.
bool normalize_resources(CallContract &value) {
    for (uint32_t index = 0; index < value.resource_count; ++index) {
        const auto resource = value.resources[index];
        if (resource.id.size() == 0U || resource.units == 0U) {
            return false;
        }
        uint32_t at = index;
        while (at != 0U &&
               compare_tokens(view(resource.id), view(value.resources[at - 1U].id)) < 0) {
            value.resources[at] = value.resources[at - 1U];
            --at;
        }
        if (at != 0U && resource.id.equals(view(value.resources[at - 1U].id))) {
            return false;
        }
        value.resources[at] = resource;
    }
    // Unused cells have no semantic meaning and retain no caller data.
    for (uint32_t index = value.resource_count; index < resource_group_max; ++index) {
        value.resources[index] = {};
    }
    return true;
}
} // namespace

ayni_status_t decode_call_capability(const ayni_call_capability_v1 &input, CallCapability &out) {
    const auto status = check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (status != AYNI_OK) {
        return status;
    }
    CallCapability value{};
    if (input.version == 0U || !value.id.assign(input.capability_id)) {
        return AYNI_INVALID_ARGUMENT;
    }
    value.version = input.version;
    const auto contract_status = decode_contract(input.contract, value.contract);
    if (contract_status == AYNI_OK) {
        out = value;
    }
    return contract_status;
}

// Static compatibility is independent of runtime authority or resource health.
// PHYSICAL/subscription are recognizable declarations; the M1 consumer must
// reject unsupported execution until its permit/subscription machinery exists.
ayni_status_t normalize_call_contract(const CallContract &input, CallContract &out) {
    if (!known_form(input.form) || !known_class(input.category) ||
        (input.effects & ~call_effect_mask) != 0U || !known_idempotency(input.idempotency) ||
        !known_query(input.query) || !known_cancel(input.cancel) || input.timeout_ms == 0U ||
        input.input_text_limit > call_text_limit_max ||
        input.result_text_limit > call_text_limit_max ||
        input.resource_count > resource_group_max) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (input.input_format != call_fields_text_input_format ||
        input.result_format != call_text_result_format) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    CallContract value = input;
    if (!normalize_resources(value)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = value;
    return AYNI_OK;
}

// Exact semantics, not a permissive subset comparison: changing a timeout,
// destination-related field, effect or size bound requires deliberate rebinding.
bool same_call_contract(const CallContract &left, const CallContract &right) {
    if (left.form != right.form || left.category != right.category ||
        left.effects != right.effects || left.idempotency != right.idempotency ||
        left.query != right.query || left.cancel != right.cancel ||
        left.dedupe_window_ms != right.dedupe_window_ms || left.timeout_ms != right.timeout_ms ||
        left.input_format != right.input_format || left.result_format != right.result_format ||
        left.input_text_limit != right.input_text_limit ||
        left.result_text_limit != right.result_text_limit ||
        left.resource_count != right.resource_count) {
        return false;
    }
    for (uint32_t index = 0; index < left.resource_count; ++index) {
        if (!left.resources[index].id.equals(view(right.resources[index].id)) ||
            left.resources[index].units != right.resources[index].units) {
            return false;
        }
    }
    return true;
}

ayni_status_t CallCapabilityRegistry::add(const CallCapability &capability) {
    if (capability.id.size() == 0U || capability.version == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    CallCapability value = capability;
    const auto status = normalize_call_contract(capability.contract, value.contract);
    if (status != AYNI_OK) {
        return status;
    }
    return add_validated(value);
}

ayni_status_t CallCapabilityRegistry::add_validated(const CallCapability &value) {
    uint32_t index = 0;
    if (find(view(value.id), value.version, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return entries_.push_back(value) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
}

bool CallCapabilityRegistry::find(ayni_string_v1 id, uint32_t version, uint32_t &index) const {
    for (uint32_t at = 0; at < entries_.size(); ++at) {
        if (entries_[at].id.equals(id) && entries_[at].version == version) {
            index = at;
            return true;
        }
    }
    return false;
}
} // namespace ayni::catalog
