#include "work/goal/calendar.h"
#include "catalog/abi_checks.h"
#include <cstring>
#include <limits>

namespace ayni::work {
namespace {
constexpr uint64_t microseconds_per_second = 1000000;
} // namespace
int64_t calendar_period_s(ayni_calendar_kind_t kind) {
    return kind == AYNI_CALENDAR_DAILY    ? calendar_day_s
           : kind == AYNI_CALENDAR_WEEKLY ? calendar_week_days * calendar_day_s
                                          : 0;
}

ayni_status_t decode_calendar_rule(const ayni_calendar_rule_v1 &input, CalendarRule &out) {
    CalendarRule rule{};
    const auto shape = catalog::check_struct_size(input.struct_size, sizeof(input), sizeof(input));
    if (shape != AYNI_OK) {
        return shape;
    }
    if (input.rule_version != calendar_rule_version) {
        return AYNI_UNSUPPORTED_VERSION;
    }
    if (input.kind < AYNI_CALENDAR_ONCE || input.kind > AYNI_CALENDAR_WEEKLY ||
        input.first_local_s < 0 || input.first_local_s > AYNI_CALENDAR_SECONDS_MAX ||
        !parse_calendar_zone(input.timezone, input.timezone_size, rule.zone)) {
        return AYNI_INVALID_ARGUMENT;
    }
    int64_t first = 0;
    if (!calendar_to_utc(rule.zone, input.first_local_s, first)) {
        return AYNI_INVALID_ARGUMENT;
    }
    rule.kind = input.kind;
    rule.version = input.rule_version;
    rule.first_local_s = input.first_local_s;
    rule.timezone_size = input.timezone_size;
    std::memcpy(rule.timezone, input.timezone, rule.timezone_size);
    out = rule;
    return AYNI_OK;
}

ayni_calendar_rule_v1 calendar_rule_info(const CalendarRule &rule) {
    ayni_calendar_rule_v1 out{};
    out.struct_size = sizeof(out);
    out.kind = rule.kind;
    out.rule_version = rule.version;
    out.first_local_s = rule.first_local_s;
    out.timezone_size = rule.timezone_size;
    std::memcpy(out.timezone, rule.timezone, rule.timezone_size);
    return out;
}
bool same_calendar_rule(const CalendarRule &left, const CalendarRule &right) {
    return left.kind == right.kind && left.version == right.version &&
           left.first_local_s == right.first_local_s && left.timezone_size == right.timezone_size &&
           std::memcmp(left.timezone, right.timezone, left.timezone_size) == 0;
}

int64_t calendar_next(const CalendarRule &rule, int64_t last_local_s) {
    if (rule.kind == 0 || (rule.kind == AYNI_CALENDAR_ONCE && last_local_s >= rule.first_local_s)) {
        return AYNI_CALENDAR_TIME_NONE;
    }
    auto next = rule.first_local_s;
    if (last_local_s >= next) {
        const auto spacing = calendar_period_s(rule.kind);
        next += ((last_local_s - next) / spacing + 1) * spacing;
    }
    int64_t utc = 0;
    return next <= AYNI_CALENDAR_SECONDS_MAX && calendar_to_utc(rule.zone, next, utc)
               ? utc
               : AYNI_CALENDAR_TIME_NONE;
}

// Arithmetic skips arbitrarily long backlogs in constant work. A bounded
// neighbouring-date adjustment accounts for POSIX offset changes near midnight.
CalendarDue calendar_due(const CalendarRule &rule, int64_t last_local_s, CalendarClock clock) {
    CalendarDue due{};
    if (!clock.trusted || rule.kind == 0) {
        return due;
    }
    const auto standard_local = clock.utc_s - rule.zone.standard;
    const auto daylight_local = clock.utc_s - rule.zone.daylight;
    const auto high = standard_local > daylight_local ? standard_local : daylight_local;
    const auto local_high = high > AYNI_CALENDAR_SECONDS_MAX ? AYNI_CALENDAR_SECONDS_MAX : high;
    const auto spacing = calendar_period_s(rule.kind);
    auto latest = rule.first_local_s;
    if (rule.kind != AYNI_CALENDAR_ONCE) {
        if (local_high < latest) {
            return due;
        }
        latest += (local_high - latest) / spacing * spacing;
    }
    int64_t utc = 0;
    // Two legal POSIX offsets differ by fewer than three civil days. Trying
    // four candidates also catches an already-passed first fold occurrence
    // while the displayed clock is still before its repeated local time.
    constexpr uint32_t candidate_limit = 4;
    for (uint32_t candidate = 0; candidate < candidate_limit; ++candidate) {
        if (!calendar_to_utc(rule.zone, latest, utc)) {
            return due;
        }
        if (utc <= clock.utc_s) {
            break;
        }
        if (rule.kind == AYNI_CALENDAR_ONCE || latest - rule.first_local_s < spacing) {
            return due;
        }
        latest -= spacing;
    }
    if (latest <= last_local_s || utc > clock.utc_s) {
        return due;
    }
    const auto first =
        last_local_s < rule.first_local_s ? rule.first_local_s : last_local_s + spacing;
    const auto count = rule.kind == AYNI_CALENDAR_ONCE ? 1 : (latest - first) / spacing + 1;
    due = {latest, utc, static_cast<uint32_t>(count > UINT32_MAX ? UINT32_MAX : count)};
    return due;
}

uint64_t calendar_monotonic(int64_t utc_s, CalendarClock clock) {
    if (!clock.trusted || utc_s < 0) {
        return UINT64_MAX;
    }
    if (utc_s <= clock.utc_s) {
        return clock.monotonic_us;
    }
    const auto seconds = static_cast<uint64_t>(utc_s - clock.utc_s);
    return seconds > (UINT64_MAX - clock.monotonic_us) / microseconds_per_second
               ? UINT64_MAX
               : clock.monotonic_us + seconds * microseconds_per_second;
}
} // namespace ayni::work
