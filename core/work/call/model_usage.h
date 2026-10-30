// Portable retained usage footprint; validation is shared with SDK consumers.
#ifndef AYNI_CORE_WORK_CALL_MODEL_USAGE_H
#define AYNI_CORE_WORK_CALL_MODEL_USAGE_H
#include <ayni/detail/model_usage.hpp>
namespace ayni::work {
// Portable kind/reserved and three u64 counters, contracts/storage/records.md.
inline constexpr uint32_t model_usage_record_bytes = 32;
using ::ayni::detail::valid_model_usage;
} // namespace ayni::work
#endif
