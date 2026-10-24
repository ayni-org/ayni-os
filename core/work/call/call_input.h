// Canonical concrete arguments plus bounded conversation text (ADR-0042/0045).
// The same argument bytes are authorized and delivered; there is no parallel
// permission projection. JSON and credentials belong outside this representation.
#ifndef AYNI_CORE_WORK_CALL_CALL_INPUT_H
#define AYNI_CORE_WORK_CALL_CALL_INPUT_H

#include "catalog/capability/call_contract.h"
#include <ayni/authority.h>
#include <cstdint>

namespace ayni::work {

// Typed input borrowed through encode_call_input; caller must not mutate views.
struct CallInput {
    ayni_scope_v1 arguments; // All declared fields, one concrete value each; never a ceiling.
    ayni_string_v1 text;     // UTF-8 body bytes, not NUL-terminated; empty text is permitted.
};

// Decoded immutable views borrow one owned canonical input body.
struct CallInputView {
    ayni_bytes_v1 arguments; // Canonical complete singleton fields; domain format 1.
    ayni_string_v1 text;     // Validated bounded UTF-8 bytes; may include U+0000.
};

// Format 1, contracts/storage/records.md: two u32 lengths, arguments, text.
constexpr uint32_t call_input_header_size = 8;

// Text limit is 0..catalog::call_text_limit_max; zero permits only empty text. Output
// does not overlap domain or any input view. BUFFER_TOO_SMALL only sets size;
// any other failure leaves all outputs unchanged. This is a business payload,
// never an error/log message; persistence and deletion are owned by work/call.
ayni_status_t encode_call_input(ayni_bytes_v1 domain, const CallInput &input, uint32_t text_limit,
                                uint8_t *out, uint32_t capacity, uint32_t &out_size);
// A saved input uses its original domain, not a replacement catalog's schema.
// Refuses non-singleton/missing arguments and noncanonical framing, and changes
// no output on failure. This validates shape, not Grant coverage or use consent.
ayni_status_t read_call_input(ayni_bytes_v1 domain, ayni_bytes_v1 body, uint32_t text_limit,
                              CallInputView &out);

} // namespace ayni::work
#endif
