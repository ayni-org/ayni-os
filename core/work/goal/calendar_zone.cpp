#include "work/goal/calendar_zone.h"
#include <cstddef>

namespace ayni::work {
namespace {
constexpr int32_t months_per_year = 12;
constexpr int32_t ordinary_year_days = 365;
constexpr int32_t leap_cycle = 4;
constexpr int32_t century = 100;
constexpr int32_t gregorian_cycle = 400;
constexpr int32_t epoch_year = 1970;
constexpr int32_t epoch_weekday = 4; // 1970-01-01 was Thursday, Sunday = 0.
constexpr int32_t last_week = 5;
constexpr int32_t february = 2;
constexpr int32_t leap_day_number = 60;
constexpr int32_t offset_hour_max = 24;
constexpr int32_t transition_hour_max = 167;
constexpr int32_t minute_second_max = 59;
constexpr uint32_t name_min = 3;
constexpr int32_t year_search_min = 1968;
constexpr int32_t year_search_end = 10002;
constexpr int32_t transition_neighbours = 1;
constexpr int32_t binary_partitions = 2;
constexpr int32_t month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
constexpr int64_t epoch_leap_days =
    (epoch_year - 1) / leap_cycle - (epoch_year - 1) / century + (epoch_year - 1) / gregorian_cycle;

bool leap(int32_t year) {
    return year % leap_cycle == 0 && (year % century != 0 || year % gregorian_cycle == 0);
}
int32_t days_in_month(int32_t year, int32_t month) {
    return month_days[month - 1] + (month == february && leap(year) ? 1 : 0);
}
int64_t floor_day(int64_t seconds) {
    return seconds / calendar_day_s - (seconds < 0 && seconds % calendar_day_s != 0 ? 1 : 0);
}
int32_t year_at(int64_t seconds) {
    const auto day = floor_day(seconds);
    int32_t first = year_search_min;
    int32_t end = year_search_end;
    while (first + 1 < end) {
        const auto middle = first + (end - first) / binary_partitions;
        if (calendar_days(middle, 1, 1) <= day) {
            first = middle;
        } else {
            end = middle;
        }
    }
    return first;
}

// One bounded input cursor; offsets and transitions share the h:m:s parser.
class Parser {
  public:
    Parser(const char *bytes, uint32_t size) : bytes_(bytes), size_(size) {}
    bool done() const { return at_ == size_; }
    bool take(char byte) {
        if (at_ < size_ && bytes_[at_] == byte) {
            ++at_;
            return true;
        }
        return false;
    }
    bool number(int32_t maximum, int32_t &out) {
        const auto before = at_;
        int32_t value = 0;
        while (at_ < size_ && bytes_[at_] >= '0' && bytes_[at_] <= '9') {
            constexpr int32_t decimal_base = 10;
            const auto digit = bytes_[at_++] - '0';
            if (value > (maximum - digit) / decimal_base || digit > maximum) {
                return false;
            }
            value = value * decimal_base + digit;
        }
        out = value;
        return at_ > before;
    }
    bool name() {
        const bool quoted = take('<');
        const auto before = at_;
        while (at_ < size_) {
            const char byte = bytes_[at_];
            const bool alpha = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z');
            const bool extra =
                quoted && ((byte >= '0' && byte <= '9') || byte == '+' || byte == '-');
            if (!alpha && !extra) {
                break;
            }
            ++at_;
        }
        return at_ - before >= name_min && (!quoted || take('>'));
    }
    bool time(int32_t hour_max, int32_t &out) {
        const auto sign = take('-') ? -1 : 1;
        if (sign > 0) {
            static_cast<void>(take('+'));
        }
        int32_t hours = 0;
        int32_t minutes = 0;
        int32_t seconds = 0;
        if (!number(hour_max, hours) ||
            (take(':') && (!number(minute_second_max, minutes) ||
                           (take(':') && !number(minute_second_max, seconds))))) {
            return false;
        }
        out = sign *
              static_cast<int32_t>(hours * calendar_hour_s + minutes * calendar_minute_s + seconds);
        return true;
    }
    bool transition(ZoneTransition &out) {
        ZoneTransition rule{};
        if (take('M')) {
            rule.kind = ZoneTransition::Kind::month_week_day;
            if (!number(months_per_year, rule.a) || rule.a == 0 || !take('.') ||
                !number(last_week, rule.b) || rule.b == 0 || !take('.') ||
                !number(calendar_week_days - 1, rule.c)) {
                return false;
            }
        } else if (take('J')) {
            rule.kind = ZoneTransition::Kind::julian;
            if (!number(ordinary_year_days, rule.a) || rule.a == 0) {
                return false;
            }
        } else if (!number(ordinary_year_days, rule.a)) {
            return false;
        }
        if (take('/') && !time(transition_hour_max, rule.seconds)) {
            return false;
        }
        out = rule;
        return true;
    }

  private:
    const char *bytes_; // Borrowed immutable rule; valid through parsing only.
    uint32_t size_;     // At most the public TZ byte bound.
    uint32_t at_ = 0;   // First unread byte, always <= size_.
};

int64_t transition_local(const ZoneTransition &rule, int32_t year) {
    auto day = calendar_days(year, 1, 1);
    if (rule.kind == ZoneTransition::Kind::day) {
        day += rule.a;
    } else if (rule.kind == ZoneTransition::Kind::julian) {
        day += rule.a - 1 + (leap(year) && rule.a >= leap_day_number ? 1 : 0);
    } else {
        day = calendar_days(year, rule.a, 1);
        const auto weekday = static_cast<int32_t>((day + epoch_weekday) % calendar_week_days);
        auto date = 1 + (rule.c - weekday + calendar_week_days) % calendar_week_days +
                    (rule.b - 1) * calendar_week_days;
        if (date > days_in_month(year, rule.a)) {
            date -= calendar_week_days; // POSIX week 5 means the last matching weekday.
        }
        day += date - 1;
    }
    return day * calendar_day_s + rule.seconds;
}
struct Transition {
    int64_t utc;    // Instant of offset change in Unix seconds.
    int32_t before; // Seconds west before the transition.
    int32_t after;  // Seconds west after the transition.
};
template <typename Visit> void transitions(const CalendarZone &zone, int32_t year, Visit visit) {
    for (auto nearby = year - transition_neighbours; nearby <= year + transition_neighbours;
         ++nearby) {
        visit(Transition{transition_local(zone.start, nearby) + zone.standard, zone.standard,
                         zone.daylight});
        visit(Transition{transition_local(zone.end, nearby) + zone.daylight, zone.daylight,
                         zone.standard});
    }
}
} // namespace

int64_t calendar_days(int32_t year, int32_t month, int32_t day) {
    int64_t days = int64_t{year - epoch_year} * ordinary_year_days + (year - 1) / leap_cycle -
                   (year - 1) / century + (year - 1) / gregorian_cycle - epoch_leap_days;
    for (int32_t before = 1; before < month; ++before) {
        days += days_in_month(year, before);
    }
    return days + day - 1;
}

// Share the scheduler's Gregorian arithmetic with public civil views. A bounded
// month walk avoids a second date algorithm in notification and other applications.
bool calendar_fields(int64_t local_s, ayni_calendar_civil_v1 &out) {
    if (local_s < 0 || local_s > AYNI_CALENDAR_SECONDS_MAX) {
        return false;
    }
    ayni_calendar_civil_v1 fields{};
    fields.struct_size = sizeof(fields);
    fields.year = year_at(local_s);
    fields.month = 1;
    auto day = local_s / calendar_day_s - calendar_days(fields.year, fields.month, 1);
    while (day >= days_in_month(fields.year, fields.month)) {
        day -= days_in_month(fields.year, fields.month);
        ++fields.month;
    }
    fields.day = static_cast<int32_t>(day) + 1;
    const auto seconds = local_s % calendar_day_s;
    fields.hour = static_cast<int32_t>(seconds / calendar_hour_s);
    fields.minute = static_cast<int32_t>((seconds % calendar_hour_s) / calendar_minute_s);
    fields.second = static_cast<int32_t>(seconds % calendar_minute_s);
    out = fields;
    return true;
}

bool parse_calendar_zone(const char *bytes, uint32_t size, CalendarZone &out) {
    if (bytes == nullptr || size == 0 || size > AYNI_CALENDAR_TZ_MAX_BYTES) {
        return false;
    }
    Parser parser(bytes, size);
    CalendarZone zone{};
    if (!parser.name() || !parser.time(offset_hour_max, zone.standard)) {
        return false;
    }
    zone.daylight = zone.standard;
    if (!parser.done()) {
        zone.has_daylight = true;
        zone.daylight = zone.standard - calendar_hour_s;
        if (!parser.name()) {
            return false;
        }
        if (!parser.take(',')) {
            if (!parser.time(offset_hour_max, zone.daylight) || !parser.take(',')) {
                return false;
            }
        }
        if (!parser.transition(zone.start) || !parser.take(',') || !parser.transition(zone.end)) {
            return false;
        }
    }
    if (!parser.done()) {
        return false;
    }
    out = zone;
    return true;
}

int32_t calendar_offset(const CalendarZone &zone, int64_t utc_s) {
    if (!zone.has_daylight) {
        return zone.standard;
    }
    int64_t latest = INT64_MIN;
    auto offset = zone.standard;
    transitions(zone, year_at(utc_s), [&](Transition change) {
        if (change.utc <= utc_s && change.utc > latest) {
            latest = change.utc;
            offset = change.after;
        }
    });
    return offset;
}

// Try both offsets to resolve folds without libc's process-global TZ. If neither
// describes the requested fields, only a forward gap may map them (C-TIME-006).
bool calendar_to_utc(const CalendarZone &zone, int64_t local_s, int64_t &out_utc_s) {
    int64_t selected = INT64_MAX;
    const int32_t offsets[] = {zone.standard, zone.daylight};
    for (auto offset : offsets) {
        const auto candidate = local_s + offset;
        if (calendar_offset(zone, candidate) == offset && candidate < selected) {
            selected = candidate;
        }
    }
    if (selected == INT64_MAX && zone.has_daylight) {
        transitions(zone, year_at(local_s), [&](Transition change) {
            const auto before = change.utc - change.before;
            const auto after = change.utc - change.after;
            if (before <= local_s && local_s < after && change.utc < selected) {
                selected = change.utc;
            }
        });
    }
    if (selected < 0 || selected > AYNI_CALENDAR_SECONDS_MAX) {
        return false;
    }
    out_utc_s = selected;
    return true;
}
} // namespace ayni::work
