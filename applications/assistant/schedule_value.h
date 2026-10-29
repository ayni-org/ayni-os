// Model arguments are structured values; the owner CLI grammar stays in schedule.cpp.
#ifndef AYNI_ASSISTANT_SCHEDULE_VALUE_H
#define AYNI_ASSISTANT_SCHEDULE_VALUE_H
#include <ayni/applications/assistant.h>
#include <ayni/values.h>
namespace ayni::applications::assistant::detail {
enum ScheduleField : uint32_t { kind = 1, frequency, local_time, timezone, body, period_seconds };
inline constexpr uint64_t seconds_to_us = 1000000;
inline constexpr uint64_t minimum_period_seconds = check_period_us / seconds_to_us;
// Requires the contract-normalized object. Cross-field rules reject irrelevant operands,
// rather than silently dropping them. Failure leaves out unchanged.
ayni_status_t read_schedule(ayni_value_v1, Schedule &out);
} // namespace ayni::applications::assistant::detail
#endif
