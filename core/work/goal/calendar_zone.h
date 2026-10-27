// Pure bounded POSIX TZ interpretation (ADR-0061); no libc TZ or platform dependency.
#ifndef AYNI_CORE_WORK_GOAL_CALENDAR_ZONE_H
#define AYNI_CORE_WORK_GOAL_CALENDAR_ZONE_H
#include <ayni/calendar.h>
#include <cstdint>

namespace ayni::work {
inline constexpr int64_t calendar_day_s = 86400;
inline constexpr int64_t calendar_hour_s = 3600;
inline constexpr int64_t calendar_minute_s = 60;
inline constexpr int32_t calendar_week_days = 7;
inline constexpr int32_t calendar_transition_default_hour = 2;

// One annual transition: M.month.week.weekday, Julian day excluding leap day,
// or zero-based day including leap day. seconds is relative to local midnight.
struct ZoneTransition {
    enum class Kind { month_week_day, julian, day };
    Kind kind = Kind::day; // Representation selects which of a/b/c has meaning.
    int32_t a = 0;         // Month 1..12, Julian 1..365, or day 0..365.
    int32_t b = 0;         // M: week 1..5 (5 means last); zero otherwise.
    int32_t c = 0;         // M: Sunday 0..Saturday 6; zero otherwise.
    int32_t seconds = calendar_transition_default_hour *
                      calendar_hour_s; // Signed local transition time, bounded to 167:59:59.
};
// Parsed rule owned by a calendar value; offsets are seconds west of UTC.
struct CalendarZone {
    int32_t standard = 0;      // Standard local + standard = UTC.
    int32_t daylight = 0;      // Equal to standard when no daylight rule exists.
    bool has_daylight = false; // True iff both start/end transitions were parsed.
    ZoneTransition start{};    // Ignored without daylight; interpreted in standard local time.
    ZoneTransition end{};      // Ignored without daylight; interpreted in daylight local time.
};

// Exact ASCII POSIX TZ subset: names/offsets and explicit M/J/n transition rules.
// Empty, colon/IANA paths, omitted DST rules and trailing bytes are rejected.
bool parse_calendar_zone(const char *bytes, uint32_t size, CalendarZone &out);
// Signed epoch day; internal neighbouring-year arithmetic also uses days before 1970.
int64_t calendar_days(int32_t year, int32_t month, int32_t day);
// Seconds west of UTC at this instant; bounded neighbouring annual transitions.
int32_t calendar_offset(const CalendarZone &zone, int64_t utc_s);
// Fold chooses the earlier instant; a missing civil time moves to the first valid
// instant after the gap (C-TIME-005/006). False preserves out_utc_s.
bool calendar_to_utc(const CalendarZone &zone, int64_t local_s, int64_t &out_utc_s);
} // namespace ayni::work
#endif
