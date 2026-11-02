#include "catalog/capability/call_contract.h"
#include "foundation/workspace.h"

#include "catalog/capability/business_encoding.h"
#include "catalog/capability/field_values.h"
#include <cstring>

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

// Static compatibility is independent of runtime authority or resource health.
// Live dispatch still separately checks the required execution or collection ports.
ayni_status_t normalize_call_contract(const CallContract &input, CallContract &out) {
    if (!known_form(input.form) || !known_class(input.category) ||
        (input.effects & ~call_effect_mask) != 0U || !known_idempotency(input.idempotency) ||
        !known_query(input.query) || !known_cancel(input.cancel) || input.timeout_ms == 0U ||
        input.input_text_limit > AYNI_CALL_INPUT_BYTES_MAX ||
        input.result_text_limit > AYNI_CALL_RESULT_BYTES_MAX ||
        input.resource_count > resource_group_max ||
        input.business_contract.size > AYNI_BUSINESS_CONTRACT_BYTES_MAX ||
        (input.business_contract.size != 0U && input.business_contract.data == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (input.input_format != call_value_input_format ||
        (input.result_format != call_text_result_format &&
         input.result_format != call_tool_calls_result_format &&
         input.result_format != call_value_result_format)) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    // A model proposes tool calls; a tool or an actuator never does (ADR-0054).
    if (input.result_format == call_tool_calls_result_format &&
        (input.category != CallClass::model || (input.effects & call_effect_physical) != 0U ||
         input.form == CallForm::subscription)) {
        return AYNI_INVALID_ARGUMENT;
    }
    // Collection is a Tool session, never an actuator permit or a model request.
    // The cancel port returns its borrow; finite lease still bounds collection if close is lost.
    if ((input.form == CallForm::subscription) != input.subscription.has_value() ||
        (input.subscription &&
         (input.category != CallClass::tool || (input.effects & call_effect_physical) != 0 ||
          input.cancel != CancelMode::confirmed || input.subscription->source.size() == 0 ||
          input.subscription->source_version == 0 || input.subscription->lease_ms == 0))) {
        return AYNI_INVALID_ARGUMENT;
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
namespace {
bool same_call_semantics(const CallContract &left, const CallContract &right) {
    if (left.form != right.form || left.category != right.category ||
        left.effects != right.effects || left.idempotency != right.idempotency ||
        left.query != right.query || left.cancel != right.cancel ||
        left.dedupe_window_ms != right.dedupe_window_ms || left.timeout_ms != right.timeout_ms ||
        left.input_format != right.input_format || left.result_format != right.result_format ||
        left.input_text_limit != right.input_text_limit ||
        left.result_text_limit != right.result_text_limit ||
        left.resource_count != right.resource_count ||
        left.subscription.has_value() != right.subscription.has_value() ||
        left.business_contract.size != right.business_contract.size ||
        (left.business_contract.size != 0U &&
         std::memcmp(left.business_contract.data, right.business_contract.data,
                     left.business_contract.size) != 0)) {
        return false;
    }
    if (left.subscription &&
        (!left.subscription->source.equals(view(right.subscription->source)) ||
         left.subscription->source_version != right.subscription->source_version ||
         left.subscription->lease_ms != right.subscription->lease_ms)) {
        return false;
    }
    return true;
}
} // namespace

bool same_call_contract(const CallContract &left, const CallContract &right) {
    if (!same_call_semantics(left, right)) {
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

// ADR-0087: the saved logical proof and effective group have an allowed bijection.
// Resource order is canonical independently on each side; units cannot disappear,
// merge or change. Exact current Binding compatibility is checked before dispatch.
bool bound_call_contract(const CallContract &logical, const CallContract &effective) {
    if (!same_call_semantics(logical, effective)) {
        return false;
    }
    std::array<bool, resource_group_max> matched{};
    for (uint32_t logical_at = 0; logical_at < logical.resource_count; ++logical_at) {
        bool found = false;
        for (uint32_t actual = 0; actual < effective.resource_count; ++actual) {
            if (!matched[actual] &&
                logical.resources[logical_at].units == effective.resources[actual].units) {
                matched[actual] = true;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

ayni_status_t CallCapabilityRegistry::add(ayni::foundation::Workspace &workspace,
                                          const CallCapability &capability) {
    if (capability.id.size() == 0U || capability.version == 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    CallCapability value = capability;
    const auto status = normalize_call_contract(capability.contract, value.contract);
    if (status != AYNI_OK) {
        return status;
    }
    return add_validated(workspace, value);
}

ayni_status_t CallCapabilityRegistry::add_validated(ayni::foundation::Workspace &workspace,
                                                    const CallCapability &value,
                                                    const ayni_business_contract_v1 *definition) {
    uint32_t index = 0;
    if (find(view(value.id), value.version, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (entries_.full()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    CallCapability candidate = value;
    const auto stored =
        store_business_contract(workspace, value.contract, definition,
                                proofs_ + static_cast<std::size_t>(entries_.size()) * proof_bytes_,
                                proof_bytes_, candidate.contract.business_contract);
    if (stored != AYNI_OK) {
        return stored;
    }
    return entries_.push_back(candidate) ? AYNI_OK : AYNI_CAPACITY_EXCEEDED;
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
