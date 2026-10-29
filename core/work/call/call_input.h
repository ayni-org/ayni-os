// Canonical business values and their original machine proof (ADR-0084).
// Authority/control operands are derived from this single owned request.
// JSON and credentials belong outside this representation.
#ifndef AYNI_CORE_WORK_CALL_CALL_INPUT_H
#define AYNI_CORE_WORK_CALL_CALL_INPUT_H

#include "catalog/capability/call_contract.h"
#include "catalog/capability/value_encoding.h"
#include <ayni/authority.h>
#include <cstdint>

namespace ayni::work {

// Decoded immutable views borrow one owned canonical input body.
struct CallInputView {
    CallInputView() = default;                  // Empty decoding scratch, with no borrowed request.
    CallInputView(const CallInputView &source); // Copies rebase the first-row projection view.
    CallInputView &operator=(const CallInputView &source); // Same ownership rule as construction.
    ayni_bytes_v1 arguments;     // Canonical complete singleton fields; domain format 1.
    catalog::DecodedValue value; // Typed input's nodes; text borrows its canonical body.
    std::array<uint8_t, AYNI_AUTHORITY_SCOPE_MAX_BYTES_MAX>
        projection{}; // First row for fixed physical control only.
};

// Shared UTF-8 result/protocol-ID validation. No truncation, normalization or logging.
ayni_status_t check_call_text(ayni_string_v1 text, uint32_t limit);

// The same-boundary normalized tree must fit its capability's complete input
// byte limit, including defaults/framing; shared by direct requests and proposals.
ayni_status_t check_business_input_limit(const catalog::CallContract &contract,
                                         const ayni_value_v1 &normalized);
// Schema-normalized business input; output holds only value bytes, no caller-supplied authority.
ayni_status_t encode_business_input(ayni_bytes_v1 domain, const catalog::CallContract &contract,
                                    const ayni_value_v1 &input, uint8_t *out, uint32_t capacity,
                                    uint32_t &size);
// Original proof interprets saved values. First-row control view is reconstructed,
// while authorization independently proves every row under one saved requirement.
ayni_status_t read_call_input(ayni_bytes_v1 domain, ayni_bytes_v1 body,
                              const catalog::CallContract &contract, CallInputView &out);
} // namespace ayni::work
#endif
