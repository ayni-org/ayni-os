// The schedule of a goal's occurrences (ADR-0019): which times each trigger
// kind takes, when occurrences fall due, taking every due occurrence at
// once, and the ends of the clock and of the occurrence numbers.

#include "work/goal/trigger_schedule.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ayni::work::no_time;
using ayni::work::start_schedule;
using ayni::work::take_due;
using ayni::work::TakenOccurrences;
using ayni::work::Trigger;
using ayni::work::TriggerCursor;

constexpr uint64_t second = AYNI_TRIGGER_PERIOD_MIN_US;

TEST(TriggerSchedule, each_kind_takes_only_its_own_times) {
    EXPECT_TRUE((Trigger{AYNI_TRIGGER_NONE, 0, 0}.fits()));
    EXPECT_FALSE((Trigger{AYNI_TRIGGER_NONE, 1, 0}.fits()));
    EXPECT_TRUE((Trigger{AYNI_TRIGGER_ONCE, 0, 0}.fits()));
    EXPECT_FALSE((Trigger{AYNI_TRIGGER_ONCE, 0, second}.fits()));
    EXPECT_TRUE((Trigger{AYNI_TRIGGER_DELAY, 0, 0}.fits()));
    EXPECT_TRUE((Trigger{AYNI_TRIGGER_DELAY, no_time, 0}.fits()));
    EXPECT_FALSE((Trigger{AYNI_TRIGGER_DELAY, 5, second}.fits()));
    EXPECT_TRUE((Trigger{AYNI_TRIGGER_INTERVAL, 0, second}.fits()));
    EXPECT_TRUE((Trigger{AYNI_TRIGGER_INTERVAL, no_time, no_time}.fits()));
    EXPECT_FALSE((Trigger{AYNI_TRIGGER_INTERVAL, 0, second - 1U}.fits()));
    EXPECT_FALSE((Trigger{AYNI_TRIGGER_INTERVAL, 0, 0}.fits()));
}

TEST(TriggerSchedule, first_occurrence_falls_due_after_the_delay) {
    EXPECT_FALSE(start_schedule({AYNI_TRIGGER_NONE, 0, 0}, 100).has_next());
    const TriggerCursor once = start_schedule({AYNI_TRIGGER_ONCE, 0, 0}, 100);
    EXPECT_EQ(once.taken, 0U);
    EXPECT_EQ(once.next_due_us, 100U);
    EXPECT_FALSE(once.due_by(99));
    EXPECT_TRUE(once.due_by(100));
    EXPECT_EQ(start_schedule({AYNI_TRIGGER_DELAY, 50, 0}, 100).next_due_us, 150U);
    EXPECT_EQ(start_schedule({AYNI_TRIGGER_INTERVAL, 0, second}, 100).next_due_us, 100U);
}

// A time past the clock's range never comes, rather than wrapping to an
// early one.
TEST(TriggerSchedule, times_past_the_clock_never_fall_due) {
    const TriggerCursor late = start_schedule({AYNI_TRIGGER_DELAY, no_time - 10U, 0}, 100);
    EXPECT_EQ(late.next_due_us, no_time);
    EXPECT_FALSE(late.due_by(no_time));

    const Trigger huge{AYNI_TRIGGER_INTERVAL, 0, no_time - 10U};
    TriggerCursor cursor = start_schedule(huge, 100);
    const TakenOccurrences first = take_due(huge, 200, cursor);
    EXPECT_EQ(first.latest, 1U);
    EXPECT_EQ(first.due_us, 100U);
    EXPECT_FALSE(cursor.has_next());
}

TEST(TriggerSchedule, one_shot_triggers_end_after_their_occurrence) {
    for (const Trigger trigger :
         {Trigger{AYNI_TRIGGER_ONCE, 0, 0}, Trigger{AYNI_TRIGGER_DELAY, 50, 0}}) {
        TriggerCursor cursor = start_schedule(trigger, 100);
        const uint64_t due_us = cursor.next_due_us;
        const TakenOccurrences taken = take_due(trigger, due_us + 10U * second, cursor);
        EXPECT_EQ(taken.latest, 1U) << trigger.kind;
        EXPECT_EQ(taken.count, 1U) << trigger.kind;
        EXPECT_EQ(taken.due_us, due_us) << trigger.kind;
        EXPECT_EQ(cursor.taken, 1U) << trigger.kind;
        EXPECT_FALSE(cursor.has_next()) << trigger.kind;
    }
}

TEST(TriggerSchedule, interval_takes_each_occurrence_on_its_schedule) {
    const Trigger trigger{AYNI_TRIGGER_INTERVAL, 5, second};
    TriggerCursor cursor = start_schedule(trigger, 100);
    for (uint32_t number = 1; number <= 3; ++number) {
        const uint64_t due_us = 105U + (number - 1U) * second;
        ASSERT_EQ(cursor.next_due_us, due_us);
        // Taken a little late, it is still one occurrence at its own time.
        const TakenOccurrences taken = take_due(trigger, due_us + 7U, cursor);
        EXPECT_EQ(taken.latest, number);
        EXPECT_EQ(taken.count, 1U);
        EXPECT_EQ(taken.due_us, due_us);
    }
    EXPECT_EQ(cursor.next_due_us, 105U + 3U * second);
}

// C-PROD-018, C-LIMIT-007: occurrences that waited become one task, which
// names the latest; the schedule then continues where it was.
TEST(TriggerSchedule, waiting_occurrences_are_taken_together_and_the_schedule_holds) {
    const Trigger trigger{AYNI_TRIGGER_INTERVAL, 0, second};
    TriggerCursor cursor = start_schedule(trigger, 0);
    const TakenOccurrences taken = take_due(trigger, 3U * second + second / 2U, cursor);
    EXPECT_EQ(taken.latest, 4U); // at 0, 1, 2 and 3 seconds
    EXPECT_EQ(taken.count, 4U);
    EXPECT_EQ(taken.due_us, 3U * second);
    EXPECT_EQ(cursor.taken, 4U);
    EXPECT_EQ(cursor.next_due_us, 4U * second);

    const TakenOccurrences exact = take_due(trigger, 6U * second, cursor);
    EXPECT_EQ(exact.latest, 7U);
    EXPECT_EQ(exact.count, 3U);
    EXPECT_EQ(exact.due_us, 6U * second);
    EXPECT_EQ(cursor.next_due_us, 7U * second);
}

// Occurrence numbers never wrap: the last one ends the schedule.
TEST(TriggerSchedule, last_occurrence_number_ends_the_schedule) {
    const Trigger trigger{AYNI_TRIGGER_INTERVAL, 0, second};
    TriggerCursor cursor{UINT32_MAX - 3U, 1000};
    const TakenOccurrences taken = take_due(trigger, 1000U + 100U * second, cursor);
    EXPECT_EQ(taken.latest, UINT32_MAX);
    EXPECT_EQ(taken.count, 3U);
    EXPECT_EQ(taken.due_us, 1000U + 2U * second);
    EXPECT_EQ(cursor.taken, UINT32_MAX);
    EXPECT_FALSE(cursor.has_next());

    TriggerCursor almost{UINT32_MAX - 1U, 1000};
    const TakenOccurrences last = take_due(trigger, 1000, almost);
    EXPECT_EQ(last.latest, UINT32_MAX);
    EXPECT_EQ(last.count, 1U);
    EXPECT_FALSE(almost.has_next());
}

} // namespace
