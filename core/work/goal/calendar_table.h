// Calendar rules/cursors belong to work, keyed by retained Goal slot (ADR-0061).
#ifndef AYNI_CORE_WORK_GOAL_CALENDAR_TABLE_H
#define AYNI_CORE_WORK_GOAL_CALENDAR_TABLE_H
#include "foundation/bounded_array.h"
#include "foundation/transition_stamp.h"
#include "work/goal/calendar.h"
#include <optional>
namespace ayni::work {
// Ephemeral civil projection. due is empty without trusted UTC; due_since_us,
// once set, survives clock correction/trust loss until consumption or retirement.
struct CalendarPending {
    CalendarDue due{}; // This step's coalesced proposal; count 0 means none currently due.
    std::optional<uint64_t> due_since_us; // First eligible monotonic instant; never reanchored.
};
// Durable calendar row plus its separate volatile projection; goal_id 0 denotes unused.
struct CalendarRow {
    uint64_t goal_id = 0; // Stable retained Goal ID, never a slot identity.
    CalendarRule rule{};  // Immutable until the entire Goal is retired.
    int64_t last_local_s = AYNI_CALENDAR_TIME_NONE; // Last consumed planned key; -1 before first.
    int64_t last_utc_s = AYNI_CALENDAR_TIME_NONE; // Its resolved planned instant; -1 before first.
    uint32_t taken = 0; // All consumed/coalesced occurrences, equal to the Goal cursor.
    foundation::TransitionStamp stamp{}; // Last row change; zero iff unused.
    CalendarPending pending{};           // Never written to journal/snapshot; reset after recovery.
};
// Finite one-row-per-Goal store. No media, I/O or authority state is owned here.
class CalendarTable {
  public:
    static constexpr auto storage_alignment =
        foundation::BoundedArray<CalendarRow>::storage_alignment;
    static std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<CalendarRow>::storage_size(capacity);
    }
    CalendarTable(void *storage, uint32_t capacity);
    // Freshly accepted creation; coordinator already checked rule and Goal slot.
    void install(uint32_t index, uint64_t goal_id, const CalendarRule &rule,
                 foundation::TransitionStamp stamp);
    // Goal slot retirement; clears all rule/cursor bytes before reuse.
    void retire(uint32_t index) {
        if (rows_[index].goal_id != 0) {
            --occupied_;
        }
        rows_[index] = {};
    }
    bool empty() const { return occupied_ == 0; } // No retained calendar rules.
    void clear(); // Recovery failure discards every row and its volatile clock.
    // Owner-step/intent clock only; caller invalidates its scheduling cache.
    void set_clock(CalendarClock clock);
    CalendarClock clock() const { return clock_; } // The fixed frontier, never a fresh Host sample.
    const CalendarRow &at(uint32_t index) const { return rows_[index]; }
    uint32_t capacity() const { return rows_.size(); } // Same bound as Goal slots.
    CalendarDue due(uint32_t index) const;
    uint64_t deadline(uint32_t index) const;
    // Only after due(index) is nonempty; same transition as Goal/Task changes.
    CalendarDue consume(uint32_t index, foundation::TransitionStamp stamp);
    // Only after the row codec validated its rule/cursor once; check retained progression.
    bool restore_validated(uint32_t index, const CalendarRow &row);

  private:
    foundation::BoundedArray<CalendarRow>
        rows_;              // Fixed arena, one initialized entry per Goal slot.
    uint32_t occupied_ = 0; // Exactly the number of nonzero Goal IDs in rows_.
    CalendarClock clock_{}; // One ephemeral sampled frontier; never serialized or reanchored.
};
bool is_calendar_trigger(ayni_trigger_kind_t kind);
ayni_calendar_kind_t calendar_kind(ayni_trigger_kind_t kind);
// Complete cursor/identity shape, after the shared rule validator succeeded.
bool valid_calendar_cursor(const CalendarRow &row);
// One planned occurrence check shared by cursor and retained Task proofs.
bool calendar_occurrence_matches(const CalendarRule &rule, uint32_t ordinal, int64_t local_s,
                                 int64_t utc_s);
} // namespace ayni::work
#endif
