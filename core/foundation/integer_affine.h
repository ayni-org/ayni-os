// Portable checked integer projection, including 32-bit Xtensa without __int128 (ADR-0114).
#ifndef AYNI_CORE_FOUNDATION_INTEGER_AFFINE_H
#define AYNI_CORE_FOUNDATION_INTEGER_AFFINE_H
#include <cstdint>

namespace ayni::foundation {
namespace integer_affine {
constexpr uint32_t limb_bits = 32, wide_bits = 64;
constexpr uint64_t limb_mask = UINT32_MAX;
// Exact unsigned product components, independent of native wide integer support.
struct Product {
    uint64_t high; // High 64 bits of an exact unsigned 128-bit product.
    uint64_t low;  // Low 64 bits; neither component is a sentinel.
};
inline uint64_t magnitude(int64_t value) {
    return value < 0 ? uint64_t(0) - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
}
// Four 32x32 products and carries give the exact full product without signed overflow.
inline Product multiply(uint64_t left, uint64_t right) {
    const auto left_low = left & limb_mask, right_low = right & limb_mask;
    const auto left_high = left >> limb_bits, right_high = right >> limb_bits;
    const auto low_product = left_low * right_low;
    const auto carry_product = left_high * right_low + (low_product >> limb_bits);
    const auto middle = (carry_product & limb_mask) + left_low * right_high;
    return {left_high * right_high + (carry_product >> limb_bits) + (middle >> limb_bits),
            (middle << limb_bits) | (low_product & limb_mask)};
}
// Exact binary long division. Quotients exceeding 64 bits saturate before signed conversion.
inline uint64_t divide(Product product, uint64_t denominator) {
    if (product.high >= denominator) {
        return UINT64_MAX;
    }
    auto remainder = product.high;
    uint64_t quotient = 0;
    for (uint32_t bit = wide_bits; bit != 0; --bit) {
        const bool carry = (remainder >> (wide_bits - 1U)) != 0;
        remainder = (remainder << 1U) | ((product.low >> (bit - 1U)) & 1U);
        if (carry || remainder >= denominator) {
            remainder -= denominator;
            quotient |= uint64_t(1) << (bit - 1U);
        }
    }
    return quotient;
}
inline int64_t multiply_divide(int64_t input, int64_t numerator, int64_t denominator) {
    const bool negative = (input < 0) != (numerator < 0);
    const auto amount =
        divide(multiply(magnitude(input), magnitude(numerator)), uint64_t(denominator));
    const auto limit = uint64_t(INT64_MAX) + (negative ? 1U : 0U);
    if (amount >= limit) {
        return negative ? INT64_MIN : INT64_MAX;
    }
    return negative ? -static_cast<int64_t>(amount) : static_cast<int64_t>(amount);
}
inline int64_t clamp(int64_t value, int64_t minimum, int64_t maximum) {
    return value < minimum ? minimum : value > maximum ? maximum : value;
}
inline int64_t add(int64_t left, int64_t right) {
    if (right > 0 && left > INT64_MAX - right) {
        return INT64_MAX;
    }
    if (right < 0 && left < INT64_MIN - right) {
        return INT64_MIN;
    }
    return left + right;
}
} // namespace integer_affine
// Callers validate positive scale_den and ordered clamps once at rule registration.
inline int64_t map_reflex_i64(int64_t observed, int64_t input_min, int64_t input_max,
                              int64_t numerator, int64_t denominator, int64_t offset,
                              int64_t output_min, int64_t output_max) {
    const auto input = integer_affine::clamp(observed, input_min, input_max);
    const auto mapped =
        integer_affine::add(integer_affine::multiply_divide(input, numerator, denominator), offset);
    return integer_affine::clamp(mapped, output_min, output_max);
}
} // namespace ayni::foundation
#endif
