#include "foundation/utf8.h"
namespace ayni::foundation {
namespace {
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

} // namespace

// Unicode scalar UTF-8 only: no overlong encodings, surrogate code points or
// values beyond U+10FFFF. Length-delimited U+0000 is ordinary text, not a terminator.
bool valid_utf8(const char *data, uint32_t size) {
    if (data == nullptr && size != 0U) {
        return false;
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(data);
    uint32_t at = 0;
    while (at < size) {
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
        if (tails > size - at || bytes[at] < second_min || bytes[at] > second_max) {
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

} // namespace ayni::foundation
