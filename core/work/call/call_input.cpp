#include "work/call/call_input.h"
#include "foundation/workspace.h"

#include "authority/scope/scope.h"
#include "catalog/capability/business_encoding.h"
#include "catalog/capability/value_validation.h"
#include "foundation/little_endian.h"
#include "foundation/utf8.h"

#include <cstring>

namespace ayni::work {
CallInputView::CallInputView(const CallInputView &source) {
    *this = source;
}

CallInputView &CallInputView::operator=(const CallInputView &source) {
    if (this != &source) {
        arguments = source.arguments;
        value = source.value;
        projection = source.projection;
        if (source.arguments.data == source.projection.data()) {
            arguments.data = projection.data();
        }
    }
    return *this;
}

ayni_status_t check_call_text(ayni_string_v1 text, uint32_t limit) {
    if (text.size != 0U && text.data == nullptr) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (text.size > limit) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return foundation::valid_utf8(text.data, text.size) ? AYNI_OK : AYNI_INVALID_ARGUMENT;
}

ayni_status_t check_business_input_limit(const catalog::CallContract &contract,
                                         const ayni_value_v1 &normalized) {
    uint32_t required = 0;
    const auto status = catalog::measure_admitted_value(normalized, required);
    return status != AYNI_OK
               ? status
               : (required <= contract.input_text_limit ? AYNI_OK : AYNI_CAPACITY_EXCEEDED);
}
// Admission validates shape once, then shares the default/order plan used by
// public normalization. The canonical body is the sole execution input fact.
ayni_status_t encode_business_input(ayni::foundation::Workspace &workspace, ayni_bytes_v1 domain,
                                    const catalog::CallContract &contract,
                                    const ayni_value_v1 &input, uint8_t *out, uint32_t capacity,
                                    uint32_t &size, catalog::DecodedValue *normalized_out) {
    auto business_lease = workspace.make<catalog::DecodedBusiness>();
    if (!business_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &business = *business_lease;
    auto status = catalog::decode_business(workspace, domain, contract.business_contract, business);
    if (status != AYNI_OK) {
        return status;
    }
    status = catalog::validate_value(input);
    if (status != AYNI_OK) {
        return status;
    }
    auto normalized_lease = workspace.make<catalog::DecodedValue>();
    if (!normalized_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &normalized = *normalized_lease;
    status = catalog::normalize_borrowed_value(workspace, business.input.view(), input, normalized);
    if (status != AYNI_OK) {
        return status;
    }
    status = check_business_input_limit(contract, normalized.view());
    if (status != AYNI_OK) {
        return status;
    }
    status = catalog::encode_admitted_value(normalized.view(), out, capacity, size);
    if (status == AYNI_OK && normalized_out) {
        *normalized_out = normalized;
    }
    return status;
}

// Saved business input must already include defaults and canonical ordering;
// restart may validate it, but may never reinterpret or fill a new default.
ayni_status_t read_call_input(ayni::foundation::Workspace &workspace, ayni_bytes_v1 domain,
                              ayni_bytes_v1 body, const catalog::CallContract &contract,
                              CallInputView &out) {
    if (contract.input_format != catalog::call_value_input_format) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (body.size > contract.input_text_limit) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto business_lease = workspace.make<catalog::DecodedBusiness>();
    if (!business_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &business = *business_lease;
    auto status = catalog::decode_business(workspace, domain, contract.business_contract, business);
    if (status != AYNI_OK) {
        return status;
    }
    auto candidate_lease = workspace.make<CallInputView>();
    if (!candidate_lease) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    auto &candidate = *candidate_lease;
    status =
        catalog::read_normalized_value(workspace, business.input.view(), body, candidate.value);
    if (status != AYNI_OK) {
        return status;
    }
    catalog::ProjectionRow row;
    status = catalog::project_row(business.view(), candidate.value.view(), 0, row);
    if (status != AYNI_OK) {
        return status;
    }
    uint32_t projected = 0;
    status =
        authority::normalize_request(domain, row.view(), candidate.projection.data(),
                                     static_cast<uint32_t>(candidate.projection.size()), projected);
    if (status != AYNI_OK) {
        return status;
    }
    out = candidate;
    out.arguments = {out.projection.data(), projected};
    return AYNI_OK;
}
} // namespace ayni::work
