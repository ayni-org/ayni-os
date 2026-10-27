// C-TIME-004..007 and ADR-0061: civil recurrence, POSIX rules and stable coalescing.
#include "work/goal/calendar.h"
#include "work/goal/calendar_records.h"
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {
using namespace ayni::work;
int64_t civil(int year, int month, int day, int hour = 0, int minute = 0) {
    return calendar_days(year, month, day) * calendar_day_s + hour * calendar_hour_s +
           minute * calendar_minute_s;
}
CalendarRule rule(const char *tz, int64_t first, ayni_calendar_kind_t kind = AYNI_CALENDAR_DAILY) {
    ayni_calendar_rule_v1 raw{};
    raw.struct_size = sizeof(raw);
    raw.kind = kind;
    raw.rule_version = 1;
    raw.first_local_s = first;
    raw.timezone_size = static_cast<uint32_t>(std::strlen(tz));
    std::memcpy(raw.timezone, tz, raw.timezone_size);
    CalendarRule parsed{};
    EXPECT_EQ(decode_calendar_rule(raw, parsed), AYNI_OK);
    return parsed;
}
TEST(Calendar, daily_catchup_keeps_latest_planned_time_and_count) {
    const auto schedule = rule("CST-8", civil(2026, 10, 27, 14));
    const auto due =
        calendar_due(schedule, AYNI_CALENDAR_TIME_NONE, {civil(2026, 10, 30, 7), 123, true});
    EXPECT_EQ(due.local_s, civil(2026, 10, 30, 14));
    EXPECT_EQ(due.utc_s, civil(2026, 10, 30, 6));
    EXPECT_EQ(due.count, 4U);
    EXPECT_EQ(calendar_next(schedule, due.local_s), civil(2026, 10, 31, 6));
}
TEST(Calendar, weekly_schedule_is_civil_and_trust_gates_work) {
    const auto schedule = rule("UTC0", civil(2026, 10, 27, 14), AYNI_CALENDAR_WEEKLY);
    EXPECT_EQ(calendar_due(schedule, -1, {civil(2026, 11, 11), 0, false}).count, 0U);
    const auto due = calendar_due(schedule, -1, {civil(2026, 11, 11), 0, true});
    EXPECT_EQ(due.local_s, civil(2026, 11, 10, 14));
    EXPECT_EQ(due.count, 3U);
    EXPECT_EQ(calendar_due(schedule, due.local_s, {civil(2026, 11, 3), 0, true}).count, 0U);
}
TEST(Calendar, spring_gap_uses_first_valid_instant) {
    const auto schedule = rule("EST5EDT,M3.2.0/2,M11.1.0/2", civil(2026, 3, 8, 2, 30));
    int64_t utc = 0;
    ASSERT_TRUE(calendar_to_utc(schedule.zone, schedule.first_local_s, utc));
    EXPECT_EQ(utc, civil(2026, 3, 8, 7));
    EXPECT_EQ(calendar_due(schedule, -1, {utc - 1, 0, true}).count, 0U);
    EXPECT_EQ(calendar_due(schedule, -1, {utc, 0, true}).count, 1U);
}
TEST(Calendar, autumn_fold_uses_first_instant_and_never_repeats_civil_key) {
    const auto schedule = rule("EST5EDT,M3.2.0,M11.1.0", civil(2026, 11, 1, 1, 30));
    const auto first = calendar_due(schedule, -1, {civil(2026, 11, 1, 5, 30), 0, true});
    ASSERT_EQ(first.count, 1U);
    EXPECT_EQ(first.utc_s, civil(2026, 11, 1, 5, 30));
    // Still catch the passed first occurrence while the wall clock repeats 01:15.
    EXPECT_EQ(calendar_due(schedule, -1, {civil(2026, 11, 1, 6, 15), 0, true}).count, 1U);
    EXPECT_EQ(calendar_due(schedule, first.local_s, {civil(2026, 11, 1, 6, 30), 0, true}).count,
              0U);
}
TEST(Calendar, southern_hemisphere_and_fractional_offsets_are_independent) {
    const auto schedule = rule("AEST-10AEDT,M10.1.0,M4.1.0/3", civil(2026, 1, 15, 12));
    int64_t utc = 0;
    ASSERT_TRUE(calendar_to_utc(schedule.zone, schedule.first_local_s, utc));
    EXPECT_EQ(utc, civil(2026, 1, 15, 1));
    const auto east = rule("<+0545>-5:45", civil(2026, 1, 15, 12));
    ASSERT_TRUE(calendar_to_utc(east.zone, east.first_local_s, utc));
    EXPECT_EQ(utc, civil(2026, 1, 15, 6, 15));
}
TEST(Calendar, julian_and_zero_based_rules_distinguish_leap_day) {
    const auto julian = rule("STD0DST,J60/0,J300/0", civil(2028, 3, 1));
    const auto ordinal = rule("STD0DST,59/0,300/0", civil(2028, 3, 1));
    EXPECT_EQ(calendar_offset(julian.zone, civil(2028, 2, 29, 12)), 0);
    EXPECT_EQ(calendar_offset(ordinal.zone, civil(2028, 2, 29, 12)), -calendar_hour_s);
}
TEST(Calendar, ambiguous_platform_defaults_and_bad_rules_are_rejected) {
    for (const char *text :
         {"", "Europe/London", ":UTC", "EST5EDT", "UTC25", "UTC0tail", "STD0DST,M13.1.0,M11.1.0"}) {
        CalendarZone zone{};
        EXPECT_FALSE(parse_calendar_zone(text, static_cast<uint32_t>(std::strlen(text)), zone))
            << text;
    }
}
TEST(Calendar, calendar_cursor_record_roundtrips_exact_rule_and_planned_identity) {
    CalendarRow row{7,
                    rule("CST-8", civil(2026, 10, 27, 14)),
                    civil(2026, 10, 30, 14),
                    civil(2026, 10, 30, 6),
                    4,
                    {11, 9}};
    std::vector<uint8_t> bytes(calendar_record_size(row));
    write_calendar_record(row, bytes.data());
    CalendarRow decoded{};
    ASSERT_TRUE(read_calendar_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded));
    EXPECT_TRUE(same_calendar_rule(decoded.rule, row.rule));
    EXPECT_EQ(decoded.last_local_s, row.last_local_s);
    EXPECT_EQ(decoded.last_utc_s, row.last_utc_s);
    EXPECT_EQ(decoded.taken, row.taken);
}
} // namespace
