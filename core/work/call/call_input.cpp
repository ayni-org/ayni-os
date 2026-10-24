#include "work/call/call_input.h"

#include "authority/scope/scope.h"
#include "foundation/little_endian.h"

#include <cstring>

namespace ayni::work {
namespace {
constexpr uint32_t arguments_size_at = 0;
constexpr uint32_t text_size_at = sizeof(uint32_t);
constexpr uint8_t ascii_last = 0x7F;
constexpr uint8_t continuation_first = 0x80;
constexpr uint8_t continuation_last = 0xBF;
constexpr uint8_t two_byte_first = 0xC2;
constexpr uint8_t two_byte_last = 0xDF;
constexpr uint8_t three_byte_first = 0xE0;
constexpr uint8_t three_byte_last = 0xEF;
constexpr uint8_t four_byte_first = 0xF0;
constexpr uint8_t four_byte_last = 0xF4;
constexpr uint8_t surrogate_prefix = 0xED;
constexpr uint8_t three_byte_min_second = 0xA0;
constexpr uint8_t surrogate_min_second = 0xA0;
constexpr uint8_t four_byte_min_second = 0x90;
constexpr uint8_t unicode_end_second = 0x8F;
constexpr uint32_t three_byte_tail = 2;
constexpr uint32_t four_byte_tail = 3;

// Unicode scalar UTF-8 only: no overlong encodings, surrogate code points or
// values beyond U+10FFFF. Length-delimited U+0000 is ordinary text, not a terminator.
bool utf8(ayni_string_v1 text) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(text.data);
    uint32_t at = 0;
    while (at < text.size) {
        const uint8_t lead = bytes[at++];
        if (lead <= ascii_last) {
            continue;
        }
        uint32_t tails = 0;
        uint8_t second_min = continuation_first;
        uint8_t second_max = continuation_last;
        if (lead >= two_byte_first && lead <= two_byte_last) {
            tails = 1;
        } else if (lead >= three_byte_first && lead <= three_byte_last) {
            tails = three_byte_tail;
            if (lead == three_byte_first) {
                second_min = three_byte_min_second;
            } else if (lead == surrogate_prefix) {
                second_max = surrogate_min_second - 1U;
            }
        } else if (lead >= four_byte_first && lead <= four_byte_last) {
            tails = four_byte_tail;
            if (lead == four_byte_first) {
                second_min = four_byte_min_second;
            } else if (lead == four_byte_last) {
                second_max = unicode_end_second;
            }
        } else {
            return false;
        }
        if (tails > text.size - at || bytes[at] < second_min || bytes[at] > second_max) {
            return false;
        }
        ++at;
        for (uint32_t tail = 1; tail < tails; ++tail) {
            if (bytes[at] < continuation_first || bytes[at] > continuation_last) {
                return false;
            }
            ++at;
        }
    }
    return true;
}

ayni_status_t check_text(ayni_string_v1 text, uint32_t limit) {
    if (limit > call_text_max_bytes || (text.size != 0U && text.data == nullptr)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (text.size > limit) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    return utf8(text) ? AYNI_OK : AYNI_INVALID_ARGUMENT;
}
} // namespace

// Validate text before the only argument normalization pass. Reserve its entire
// length up front so a successful argument write has no later fallible step.
ayni_status_t encode_call_input(ayni_bytes_v1 domain, const CallInput &input, uint32_t text_limit,
                                uint8_t *out, uint32_t capacity, uint32_t &out_size) {
    if (out == nullptr && capacity != 0U) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto text_status = check_text(input.text, text_limit);
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
    const auto text_status = check_text(value.text, text_limit);
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
