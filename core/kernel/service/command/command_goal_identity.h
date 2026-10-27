// Shared immutable Goal echo in authorization command identities (ADR-0038).
// Byte layouts belong to command; the Goal request and state remain work-owned.
#ifndef AYNI_CORE_KERNEL_COMMAND_COMMAND_GOAL_IDENTITY_H
#define AYNI_CORE_KERNEL_COMMAND_COMMAND_GOAL_IDENTITY_H

#include "durability/batch/record_body.h"
#include "work/goal/calendar_records.h"
#include "work/goal/goal_request.h"
#include "work/goal/goal_table.h"

namespace ayni::kernel {
using work::GoalCreate;

// records.md: immutable creation scalars and three byte-string lengths.
constexpr uint32_t goal_identity_fixed_size = 60;
constexpr uint32_t goal_identity_group_size = sizeof(uint32_t); // Optional immutable group tail.
constexpr uint32_t goal_identity_catalog_ids = 2;
constexpr uint32_t goal_identity_max_bytes(uint32_t parameters) {
    return goal_identity_fixed_size + goal_identity_catalog_ids * AYNI_CATALOG_ID_MAX_BYTES +
           parameters + goal_identity_group_size + work::calendar_rule_fixed_size +
           AYNI_CALENDAR_TZ_MAX_BYTES;
}
// Full raw echo shape, independent of the current Goal/catalog retention.
bool valid_goal_creation_identity(const GoalCreate &create);
// Caller validated the enclosing command; writer owns the complete output body.
void write_goal_creation_identity(const GoalCreate &create, durability::BodyWriter &writer);
// Borrowed views; parse failures stay in reader. Enclosing command validates
// its complete shape and reader.ok() before publishing these views.
GoalCreate read_goal_creation_identity(durability::BodyReader &reader);
// Same immutable comparison for live admission and historical replay evidence.
bool matches_goal_creation(const GoalCreate &create, const work::Goal &goal,
                           ayni_bytes_v1 parameters, const work::CalendarRule *calendar = nullptr);
} // namespace ayni::kernel
#endif // AYNI_CORE_KERNEL_COMMAND_COMMAND_GOAL_IDENTITY_H
