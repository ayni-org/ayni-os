#include "work/goal/calendar_table.h"
namespace ayni::work {
bool is_calendar_trigger(ayni_trigger_kind_t kind) {
    return kind == AYNI_TRIGGER_CALENDAR_ONCE || kind == AYNI_TRIGGER_CALENDAR_DAILY ||
           kind == AYNI_TRIGGER_CALENDAR_WEEKLY;
}
ayni_calendar_kind_t calendar_kind(ayni_trigger_kind_t kind) {
    return kind == AYNI_TRIGGER_CALENDAR_ONCE     ? AYNI_CALENDAR_ONCE
           : kind == AYNI_TRIGGER_CALENDAR_DAILY  ? AYNI_CALENDAR_DAILY
           : kind == AYNI_TRIGGER_CALENDAR_WEEKLY ? AYNI_CALENDAR_WEEKLY
                                                  : 0;
}
CalendarTable::CalendarTable(void *storage, uint32_t capacity) : rows_(storage, capacity) {
    while (rows_.size() < capacity) {
        static_cast<void>(rows_.push_back({}));
    }
}
void CalendarTable::install(uint32_t index, uint64_t goal_id, const CalendarRule &rule,
                            foundation::TransitionStamp stamp) {
    if (rows_[index].goal_id == 0) {
        ++occupied_;
    }
    rows_[index] = {goal_id, rule, AYNI_CALENDAR_TIME_NONE, AYNI_CALENDAR_TIME_NONE, 0, stamp, {}};
}
void CalendarTable::clear() {
    for (uint32_t index = 0; index < rows_.size(); ++index) {
        retire(index);
    }
    clock_ = {};
}
void CalendarTable::set_clock(CalendarClock clock) {
    clock_ = clock;
    for (uint32_t index = 0; index < rows_.size(); ++index) {
        auto &row = rows_[index];
        row.pending.due =
            row.goal_id == 0 ? CalendarDue{} : calendar_due(row.rule, row.last_local_s, clock_);
        if (row.pending.due.count != 0 && !row.pending.due_since_us.has_value()) {
            row.pending.due_since_us = clock_.monotonic_us;
        }
    }
}
CalendarDue CalendarTable::due(uint32_t index) const {
    return rows_[index].pending.due;
}
uint64_t CalendarTable::deadline(uint32_t index) const {
    const auto &row = rows_[index];
    return row.goal_id == 0 ? UINT64_MAX
           : due(index).count != 0
               ? *row.pending.due_since_us
               : calendar_monotonic(calendar_next(row.rule, row.last_local_s), clock_);
}
CalendarDue CalendarTable::consume(uint32_t index, foundation::TransitionStamp stamp) {
    const auto pending = due(index);
    auto &row = rows_[index];
    row.last_local_s = pending.local_s;
    row.last_utc_s = pending.utc_s;
    row.taken += pending.count;
    row.stamp = stamp;
    row.pending = {};
    return pending;
}
bool valid_calendar_cursor(const CalendarRow &row) {
    if (row.goal_id == 0 || row.stamp.transition_id == 0 || row.stamp.commit_seq == 0) {
        return false;
    }
    if (row.taken == 0) {
        return row.last_local_s == AYNI_CALENDAR_TIME_NONE &&
               row.last_utc_s == AYNI_CALENDAR_TIME_NONE;
    }
    return calendar_occurrence_matches(row.rule, row.taken, row.last_local_s, row.last_utc_s);
}
bool calendar_occurrence_matches(const CalendarRule &rule, uint32_t ordinal, int64_t local_s,
                                 int64_t utc_s) {
    if (ordinal == 0 || (rule.kind == AYNI_CALENDAR_ONCE && ordinal != 1)) {
        return false;
    }
    const auto spacing = calendar_period_s(rule.kind);
    const auto local = rule.first_local_s + int64_t{ordinal - 1} * spacing;
    int64_t utc = 0;
    return local <= AYNI_CALENDAR_SECONDS_MAX && local_s == local &&
           calendar_to_utc(rule.zone, local, utc) && utc_s == utc;
}
bool CalendarTable::restore_validated(uint32_t index, const CalendarRow &row) {
    const auto &old = rows_[index];
    if (old.goal_id != 0 &&
        (old.goal_id != row.goal_id || !same_calendar_rule(old.rule, row.rule) ||
         row.taken <= old.taken || row.stamp.commit_seq <= old.stamp.commit_seq)) {
        return false;
    }
    if (old.goal_id == 0) {
        ++occupied_;
    }
    rows_[index] = row;
    return true;
}
} // namespace ayni::work
