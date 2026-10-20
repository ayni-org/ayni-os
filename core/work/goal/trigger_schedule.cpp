#include "work/goal/trigger_schedule.h"

namespace ayni::work {
namespace {

constexpr uint32_t last_number = UINT32_MAX;

} // namespace

uint64_t time_after(uint64_t start_us, uint64_t delay_us) {
    return delay_us > no_time - start_us ? no_time : start_us + delay_us;
}

bool Trigger::fits() const {
    switch (kind) {
    case AYNI_TRIGGER_DELAY:
        return period_us == 0U;
    case AYNI_TRIGGER_INTERVAL:
        return period_us >= AYNI_TRIGGER_PERIOD_MIN_US;
    default: // NONE, ONCE and LOCAL_INPUT take no times
        return delay_us == 0U && period_us == 0U;
    }
}

TriggerCursor start_schedule(const Trigger &trigger, uint64_t created_us) {
    // ONCE has delay_us 0, so it is due at creation. A local input trigger
    // has nothing due until an input arrives.
    const bool scheduled =
        trigger.kind != AYNI_TRIGGER_NONE && trigger.kind != AYNI_TRIGGER_LOCAL_INPUT;
    return {0, scheduled ? time_after(created_us, trigger.delay_us) : no_time};
}

TakenOccurrences take_due(const Trigger &trigger, uint64_t now_us, TriggerCursor &cursor) {
    const uint64_t first_due = cursor.next_due_us;
    if (trigger.kind == AYNI_TRIGGER_LOCAL_INPUT) {
        // record() numbered them all, so latest fits.
        const TakenOccurrences taken{cursor.taken + cursor.waiting, cursor.waiting,
                                     cursor.latest_due_us};
        cursor = {taken.latest, no_time};
        return taken;
    }
    if (trigger.kind != AYNI_TRIGGER_INTERVAL) {
        cursor = {cursor.taken + 1U, no_time};
        return {cursor.taken, 1U, first_due};
    }
    // A cursor with an occurrence to come has a number left for it, so
    // more_left is at most last_number - 1 and count below fits.
    const uint64_t more_left = last_number - cursor.taken - 1U;
    uint64_t more = (now_us - first_due) / trigger.period_us;
    if (more > more_left) {
        more = more_left;
    }
    // more * period_us <= now_us - first_due, so due_us cannot overflow.
    const uint64_t due_us = first_due + more * trigger.period_us;
    const auto count = static_cast<uint32_t>(more + 1U);
    const uint32_t latest = cursor.taken + count;
    cursor = {latest, latest == last_number ? no_time : time_after(due_us, trigger.period_us)};
    return {latest, count, due_us};
}

bool can_record(const TriggerCursor &cursor) {
    // taken + waiting never passes last_number, so this cannot wrap.
    return cursor.taken + cursor.waiting < last_number;
}

void record(uint64_t at_us, TriggerCursor &cursor) {
    if (cursor.waiting == 0U) {
        cursor.next_due_us = at_us;
    }
    ++cursor.waiting;
    cursor.latest_due_us = at_us;
}

bool occurs_again(const Trigger &trigger, const TriggerCursor &cursor) {
    return cursor.has_next() || (trigger.kind == AYNI_TRIGGER_LOCAL_INPUT && can_record(cursor));
}

} // namespace ayni::work
