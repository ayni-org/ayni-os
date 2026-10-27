// Borrowed immutable Goal creation inputs shared by live admission and command
// identity. This owner checks shape only; catalog and authority decisions remain
// with their owners. Views remain caller-owned throughout the immediate call.
#ifndef AYNI_CORE_WORK_GOAL_GOAL_REQUEST_H
#define AYNI_CORE_WORK_GOAL_GOAL_REQUEST_H
#include "work/goal/calendar.h"
#include "work/goal/trigger_schedule.h"
namespace ayni::work {

// Original creation inputs, before catalog resolution or any state mutation.
struct GoalCreate {
    ayni_string_v1 goal_type_id; // Borrowed nonempty catalog identity.
    uint32_t goal_type_version;  // Requested exact version; catalog admission checks availability.
    uint64_t owner_instance_id;  // Requested Agent instance, resolved before creation.
    uint64_t parent_goal_id;     // 0: a top-level goal
    ayni_bytes_v1 parameters;    // Borrowed bytes; null data allowed only for an empty payload.
    work::Trigger trigger;       // Kind with matching delay/period in microseconds.
    uint64_t expiry_us;          // Microseconds from creation; 0: never.
    ayni_string_v1 trigger_source; // LOCAL_INPUT: the input it takes; empty otherwise
    CalendarRule calendar{};       // CALENDAR_*: copied immutable rule; absent for all other kinds.
};

// One shape rule for ordinary creation and the approval's full original echo.
ayni_status_t check_goal_form(const GoalCreate &create);
} // namespace ayni::work
#endif
