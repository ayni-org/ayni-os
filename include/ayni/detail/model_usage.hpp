// Shared fixed-value validation for Core records and model-context consumers.
#ifndef AYNI_DETAIL_MODEL_USAGE_HPP
#define AYNI_DETAIL_MODEL_USAGE_HPP
#include <ayni/calls.h>
namespace ayni::detail {
inline bool valid_model_usage(const ayni_model_usage_v1 &usage) {
    if (usage.reserved || usage.kind > AYNI_MODEL_USAGE_INVALID) {
        return false;
    }
    if (usage.kind != AYNI_MODEL_USAGE_REPORTED) {
        return !usage.input_tokens && !usage.output_tokens && !usage.total_tokens;
    }
    return usage.input_tokens <= UINT64_MAX - usage.output_tokens &&
           usage.input_tokens + usage.output_tokens == usage.total_tokens;
}
} // namespace ayni::detail
#endif
