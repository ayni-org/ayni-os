// When a goal's occurrences fall due (ADR-0019, ADR-0021). Occurrences are
// numbered from 1 in the order they fall due, so a number names one
// occurrence of the goal for its life. Times are on the host's monotonic
// clock (C-TIME-001); they saturate at no_time instead of wrapping, and an
// occurrence at no_time never falls due. A time trigger's occurrences follow
// its schedule; a local input trigger's are recorded as its inputs arrive.
//
// Occurrences that fell due before anyone took them are taken together: one
// task stands for all of them (C-PROD-018, C-LIMIT-007), and an interval
// keeps its schedule, so the next occurrence is the first one after now.

#ifndef AYNI_CORE_WORK_GOAL_TRIGGER_SCHEDULE_H
#define AYNI_CORE_WORK_GOAL_TRIGGER_SCHEDULE_H

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::work {

constexpr uint64_t no_time = UINT64_MAX;

// start_us + delay_us, or no_time when that is past the clock's range.
uint64_t time_after(uint64_t start_us, uint64_t delay_us);

// The local input a LOCAL_INPUT trigger takes is the goal's, not part of
// the schedule (work/goal/goal_table.h).
struct Trigger {
    ayni_trigger_kind_t kind;
    uint64_t delay_us;  // DELAY and INTERVAL: from creation to the first occurrence
    uint64_t period_us; // INTERVAL: between occurrences

    // Whether delay_us and period_us fit kind, a kind the catalog defines.
    bool fits() const;
};

// Where a goal is in its schedule.
struct TriggerCursor {
    uint32_t taken; // the number of the latest occurrence taken; 0 before the first
    // When the next falls due; no_time when none will. LOCAL_INPUT: when the
    // earliest recorded occurrence not yet taken fell due, no_time while
    // none waits.
    uint64_t next_due_us;
    // LOCAL_INPUT: the occurrences recorded and not yet taken, and when the
    // latest of them fell due. A time trigger keeps the defaults.
    uint32_t waiting = 0;
    uint64_t latest_due_us = no_time;

    bool has_next() const { return next_due_us != no_time; }
    bool due_by(uint64_t now_us) const { return has_next() && next_due_us <= now_us; }
};

// The occurrences one task stands for.
struct TakenOccurrences {
    uint32_t latest; // its number
    uint32_t count;  // at least 1
    uint64_t due_us; // when the latest fell due
};

// The cursor of a goal created at created_us that has taken nothing.
TriggerCursor start_schedule(const Trigger &trigger, uint64_t created_us);

// Takes every occurrence due by now_us; only when cursor.due_by(now_us).
// Moves the cursor past them. The last number ends the schedule rather than
// wrapping.
TakenOccurrences take_due(const Trigger &trigger, uint64_t now_us, TriggerCursor &cursor);

// LOCAL_INPUT: whether one more occurrence has a number. The last number
// ends the trigger, as it ends a schedule.
bool can_record(const TriggerCursor &cursor);
// LOCAL_INPUT: records one occurrence, due at at_us, after those already
// recorded; only when can_record(cursor). at_us is no earlier than the
// times recorded before.
void record(uint64_t at_us, TriggerCursor &cursor);

} // namespace ayni::work

#endif // AYNI_CORE_WORK_GOAL_TRIGGER_SCHEDULE_H
