#include "work/call/call_input.h"

#include "authority/scope/scope.h"
#include "foundation/little_endian.h"
#include "foundation/utf8.h"

#include <cstring>

namespace ayni::work {
namespace {
constexpr uint32_t arguments_size_at = 0;
constexpr uint32_t text_size_at = sizeof(uint32_t);

} // namespace

ayni_status_t check_call_text(ayni_string_v1 text, uint32_t limit) {
    if (limit > catalog::call_text_limit_max || (text.size != 0U && text.data == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (text.size > limit) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return foundation::valid_utf8(text.data, text.size) ? AYNI_OK : AYNI_INVALID_ARGUMENT;
}

// Validate text before the only argument normalization pass. Reserve its entire
// length up front so a successful argument write has no later fallible step.
ayni_status_t encode_call_input(ayni_bytes_v1 domain, const CallInput &input, uint32_t text_limit,
                                uint8_t *out, uint32_t capacity, uint32_t &out_size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto text_status = check_call_text(input.text, text_limit);
    if (text_status != AYNI_OK) {
        return text_status;
    }
    const uint32_t framing = call_input_header_size + input.text.size;
    const bool has_framing = capacity >= framing;
    auto *argument_out = has_framing ? out + call_input_header_size : nullptr;
    const uint32_t argument_capacity = has_framing ? capacity - framing : 0U;
    uint32_t argument_size = 0;
    const auto status = authority::normalize_request(domain, input.arguments, argument_out,
                                                     argument_capacity, argument_size);
    if (status != AYNI_OK && status != AYNI_BUFFER_TOO_SMALL) {
        return status;
    }
    const uint32_t required = framing + argument_size;
    if (status == AYNI_BUFFER_TOO_SMALL) {
        out_size = required;
        return status;
    }
    foundation::store_le32(out + arguments_size_at, argument_size);
    foundation::store_le32(out + text_size_at, input.text.size);
    if (input.text.size != 0U) {
        std::memcpy(argument_out + argument_size, input.text.data, input.text.size);
    }
    out_size = required;
    return AYNI_OK;
}

// Parse lengths without unchecked addition, then share singleton/domain and
// UTF-8 validators with typed input before publishing borrowed views.
ayni_status_t read_call_input(ayni_bytes_v1 domain, ayni_bytes_v1 body, uint32_t text_limit,
                              CallInputView &out) {
    if (body.data == nullptr || body.size < call_input_header_size) {
        return AYNI_INVALID_ARGUMENT;
    }
    const uint32_t arguments_size = foundation::load_le32(body.data + arguments_size_at);
    const uint32_t text_size = foundation::load_le32(body.data + text_size_at);
    const uint32_t available = body.size - call_input_header_size;
    if (arguments_size > available || text_size != available - arguments_size) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto *arguments = body.data + call_input_header_size;
    const CallInputView value{
        {arguments, arguments_size},
        {reinterpret_cast<const char *>(arguments + arguments_size), text_size}};
    const auto text_status = check_call_text(value.text, text_limit);
    if (text_status != AYNI_OK) {
        return text_status;
    }
    const auto actual = authority::contains_request(domain, domain, value.arguments);
    if (actual.status != AYNI_OK) {
        return actual.status;
    }
    out = value;
    return AYNI_OK;
}
} // namespace ayni::work
