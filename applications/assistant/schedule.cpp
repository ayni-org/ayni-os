// Shared owner/model schedule grammar (ADR-0072). Parsing builds the same Parameters/calendar
// an owner command would; formatting is its exact inverse for owner review of model requests.
#include "policy_support.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <iterator>
#include <sstream>
namespace ayni::applications::assistant {
namespace {
using detail::text;
constexpr uint64_t microseconds_per_second = 1000000;
constexpr int64_t seconds_per_minute = 60, seconds_per_hour = 3600, seconds_per_day = 86400;
// "YYYY-MM-DDTHH:MM": fixed digit positions and separators, minute resolution.
constexpr size_t civil_bytes = 16, year_at = 0, month_at = 5, day_at = 8, hour_at = 11,
                 minute_at = 14;
constexpr size_t year_digits = 4, field_digits = 2;
constexpr int32_t months_per_year = 12;
constexpr char remind_word[] = "remind", check_word[] = "check", observe_word[] = "observe";
struct CalendarWord {
    ayni_calendar_kind_t kind;
    const char *word;
};
constexpr CalendarWord calendar_words[] = {
    {AYNI_CALENDAR_ONCE, "once"}, {AYNI_CALENDAR_DAILY, "daily"}, {AYNI_CALENDAR_WEEKLY, "weekly"}};
bool digits(const std::string &s, size_t at, size_t count, int32_t &out) {
    const auto result = std::from_chars(s.data() + at, s.data() + at + count, out);
    return result.ec == std::errc{} && result.ptr == s.data() + at + count;
}
// Days since 1970-01-01 in the proleptic Gregorian calendar, for years 1970..9999. Months are
// counted from March so the leap day ends each cycle year (Hinnant's civil algorithm).
int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    constexpr int64_t years_per_era = 400, days_per_era = 146097, days_before_epoch = 719468;
    constexpr int64_t month_scale = 153, month_bias = 2, month_divisor = 5, march_shift = 3;
    constexpr int64_t days_per_year = 365, leap_every = 4, century = 100;
    year -= month <= 2 ? 1 : 0;
    const int64_t era = year / years_per_era, of_era = year - era * years_per_era;
    const int64_t from_march =
        month > 2 ? month - march_shift : month + months_per_year - march_shift;
    const int64_t day_of_year = (month_scale * from_march + month_bias) / month_divisor + day - 1;
    const int64_t day_of_era =
        of_era * days_per_year + of_era / leap_every - of_era / century + day_of_year;
    return era * days_per_era + day_of_era - days_before_epoch;
}
// Core's own civil split is the single authority on valid dates: a value that does not
// split back to the same fields (for example February 30) is not a date.
bool civil_time(const std::string &s, int64_t &out) {
    int32_t year = 0, month = 0, day = 0, hour = 0, minute = 0;
    if (s.size() != civil_bytes || s[month_at - 1] != '-' || s[day_at - 1] != '-' ||
        s[hour_at - 1] != 'T' || s[minute_at - 1] != ':' ||
        !digits(s, year_at, year_digits, year) || !digits(s, month_at, field_digits, month) ||
        !digits(s, day_at, field_digits, day) || !digits(s, hour_at, field_digits, hour) ||
        !digits(s, minute_at, field_digits, minute) || month < 1 || month > months_per_year) {
        return false;
    }
    const int64_t local = days_from_civil(year, month, day) * seconds_per_day +
                          hour * seconds_per_hour + minute * seconds_per_minute;
    ayni_calendar_civil_v1 civil{};
    civil.struct_size = sizeof(civil);
    if (local < 0 || ayni_calendar_split_time(local, &civil) != AYNI_OK || civil.year != year ||
        civil.month != month || civil.day != day || civil.hour != hour || civil.minute != minute) {
        return false;
    }
    out = local;
    return true;
}
ayni_status_t reminder(std::istringstream &input, Schedule &out) {
    std::string kind, date, zone;
    int64_t local = 0;
    if (!(input >> kind >> date >> zone) || zone.size() > AYNI_CALENDAR_TZ_MAX_BYTES ||
        !civil_time(date, local) || detail::check_timezone(text(zone)) != AYNI_OK) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto &rule = out.calendar;
    for (const auto &entry : calendar_words) {
        if (kind == entry.word) {
            rule.kind = entry.kind;
        }
    }
    if (!rule.kind) {
        return AYNI_INVALID_ARGUMENT;
    }
    rule.struct_size = sizeof(rule);
    rule.rule_version = 1;
    rule.first_local_s = local;
    rule.timezone_size = static_cast<uint32_t>(zone.size());
    std::copy(zone.begin(), zone.end(), rule.timezone);
    // The body is everything after the zone, including later lines, so nothing is dropped.
    input >> std::ws;
    out.parameters.text.assign(std::istreambuf_iterator<char>(input), {});
    return AYNI_OK;
}
ayni_status_t check(std::istringstream &input, Schedule &out) {
    out.parameters.kind = GoalKind::device_check;
    std::string word;
    if (!(input >> word)) {
        return AYNI_OK;
    }
    uint64_t seconds = 0;
    const auto result = std::from_chars(word.data(), word.data() + word.size(), seconds);
    if (result.ec != std::errc{} || result.ptr != word.data() + word.size() || seconds == 0 ||
        seconds > UINT64_MAX / microseconds_per_second) {
        return AYNI_INVALID_ARGUMENT;
    }
    out.period_us = seconds * microseconds_per_second;
    return AYNI_OK;
}
} // namespace
ayni_status_t parse_schedule(const std::string &line, Schedule &out) {
    std::istringstream input(line);
    std::string word;
    if (!(input >> word)) {
        return AYNI_INVALID_ARGUMENT;
    }
    Schedule next;
    ayni_status_t status = AYNI_INVALID_ARGUMENT;
    if (word == remind_word) {
        next.parameters.kind = GoalKind::reminder;
        status = reminder(input, next);
    } else if (word == check_word) {
        status = check(input, next);
    } else if (word == observe_word) {
        next.parameters.kind = GoalKind::scene_observe;
        status = AYNI_OK;
    }
    // Reminder consumed the rest; every other form ends after its last optional word.
    if (status != AYNI_OK || (next.parameters.kind != GoalKind::reminder && input >> word)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = std::move(next);
    return AYNI_OK;
}
ayni_status_t format_schedule(const ayni_goal_create_v1 &goal, std::string &out) {
    Parameters p;
    auto status = decode(goal.parameters, p);
    if (status != AYNI_OK) {
        return status;
    }
    if (p.kind == GoalKind::device_check) {
        if (goal.trigger_period_us % microseconds_per_second != 0) {
            return AYNI_INVALID_ARGUMENT;
        }
        out = std::string(check_word) + ' ' +
              std::to_string(goal.trigger_period_us / microseconds_per_second);
        return AYNI_OK;
    }
    if (p.kind == GoalKind::scene_observe) {
        out = observe_word;
        return AYNI_OK;
    }
    const auto *rule = goal.calendar;
    if (p.kind != GoalKind::reminder || !rule || rule->timezone_size > AYNI_CALENDAR_TZ_MAX_BYTES) {
        return AYNI_INVALID_ARGUMENT;
    }
    const char *kind = nullptr;
    for (const auto &entry : calendar_words) {
        kind = entry.kind == rule->kind ? entry.word : kind;
    }
    ayni_calendar_civil_v1 civil{};
    civil.struct_size = sizeof(civil);
    status = ayni_calendar_split_time(rule->first_local_s, &civil);
    if (status != AYNI_OK || !kind || civil.second != 0) {
        return status == AYNI_OK ? AYNI_INVALID_ARGUMENT : status;
    }
    constexpr size_t date_bytes = civil_bytes + 1;
    char date[date_bytes]{};
    const auto written = std::snprintf(date, date_bytes, "%04d-%02d-%02dT%02d:%02d", civil.year,
                                       civil.month, civil.day, civil.hour, civil.minute);
    if (written != static_cast<int>(civil_bytes)) {
        return AYNI_INTERNAL;
    }
    out = std::string(remind_word) + ' ' + kind + ' ' + date + ' ' +
          std::string(rule->timezone, rule->timezone_size) + ' ' + p.text;
    return AYNI_OK;
}
} // namespace ayni::applications::assistant
