// Civil recurrence ownership, independent of monotonic TTLs (C-TIME-001/004..007).
#ifndef AYNI_CORE_WORK_GOAL_CALENDAR_H
#define AYNI_CORE_WORK_GOAL_CALENDAR_H
#include "work/goal/calendar_zone.h"
#include <ayni/ayni.h>

namespace ayni::work {
inline constexpr uint32_t calendar_rule_version = 1;
// Immutable value; kind 0 and all-zero fields mean no calendar rule.
struct CalendarRule {
    ayni_calendar_kind_t kind = 0;               // 0: absent; otherwise one public recurrence kind.
    uint32_t version = 0;                        // 1 iff present.
    int64_t first_local_s = 0;                   // Inclusive recurrence anchor, Gregorian seconds.
    uint32_t timezone_size = 0;                  // Exact retained POSIX rule bytes, no NUL.
    char timezone[AYNI_CALENDAR_TZ_MAX_BYTES]{}; // Private copied value, unused tail zero.
    CalendarZone zone{}; // Derived solely from timezone; never persisted separately.
};
// One fixed step reading; absent/untrusted UTC never advances civil work.
struct CalendarClock {
    int64_t utc_s = 0;         // Unix seconds, meaningful only when trusted.
    uint64_t monotonic_us = 0; // The same step's monotonic scheduling frontier.
    bool trusted = false;      // Host trust and range both passed the boundary.
};
// The latest missed occurrence and bounded coalescing count; -1 denotes an absent planned instant.
struct CalendarDue {
    int64_t local_s = AYNI_CALENDAR_TIME_NONE; // Stable civil identity; -1 means absent.
    int64_t utc_s = AYNI_CALENDAR_TIME_NONE;   // Resolved planned instant; -1 iff absent.
    uint32_t count = 0;                        // Due occurrences since the cursor; 0 iff absent.
};
// Public and stored inputs share one classified validator; failure preserves out.
ayni_status_t decode_calendar_rule(const ayni_calendar_rule_v1 &input, CalendarRule &out);
// Restore the original public representation for copied diagnostics.
ayni_calendar_rule_v1 calendar_rule_info(const CalendarRule &rule);
bool same_calendar_rule(const CalendarRule &left, const CalendarRule &right);
// Latest planned occurrence <= clock, after last_local_s; arithmetic is bounded
// independently of the number of missed days. Count saturates at UINT32_MAX.
CalendarDue calendar_due(const CalendarRule &rule, int64_t last_local_s, CalendarClock clock);
// Civil recurrence spacing; zero for ONCE/absence.
int64_t calendar_period_s(ayni_calendar_kind_t kind);
// First future instant after the cursor; -1 when a one-shot/range is exhausted.
int64_t calendar_next(const CalendarRule &rule, int64_t last_local_s);
// Project a planned instant into this step's monotonic domain, saturating at no_time.
uint64_t calendar_monotonic(int64_t utc_s, CalendarClock clock);
} // namespace ayni::work
#endif
