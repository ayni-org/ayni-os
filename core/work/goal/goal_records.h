// The GOAL and GOAL_STATE records (contracts/storage/records.md, ADR-0026).
// A transition that creates a goal writes its GOAL record, what the goal was
// created with, and every transition that changes a goal, its creation
// included, writes its GOAL_STATE record, what the change left. The stamp of
// both is their batch's. Times are on one boot's clock, so the records keep
// durations and whether a time is set, never the time.

#ifndef AYNI_CORE_WORK_GOAL_GOAL_RECORDS_H
#define AYNI_CORE_WORK_GOAL_GOAL_RECORDS_H

#include "work/goal/goal_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::work {

constexpr uint16_t goal_record_type = 3;
constexpr uint16_t goal_record_version = 1;
// Type 3/v2 keeps v1's body but requires paired type 17 authority metadata.
constexpr uint16_t goal_with_authority_record_version = 2;
// Type 3/v3 additionally admits PROPOSED and independent authorization pause.
// Layouts remain those in contracts/storage/records.md; old readers refuse.
constexpr uint16_t goal_authority_state_record_version = 3;
constexpr uint16_t goal_state_record_type = 4;
constexpr uint16_t goal_state_record_version = 1;
// Type 4/v2 carries state for an explicitly authorized Goal, including updates
// to an older v2 Goal; this protects old readers without rewriting creation.
constexpr uint16_t goal_state_with_authority_record_version = 2;
// A GOAL record without its goal type id, trigger source and parameters.
constexpr uint32_t goal_record_fixed_size = 108;
constexpr uint32_t goal_state_record_size = 48;

// What a GOAL record holds. goal.parameters points into the body, and
// goal.goal_type_index and goal.created_us are left 0.
struct GoalRecord {
    uint64_t goal_id;
    uint64_t replaced_goal_id;
    uint32_t depth;
    NewGoal goal;
    bool authority_required = false; // v2/v3 require a paired ceiling; no Goal field copy.
};

struct GoalStateRecord {
    uint64_t goal_id;
    GoalState state;
};

uint32_t goal_record_size(const Goal &goal);
// body has goal_record_size(goal) bytes; parameters are the goal's.
void write_goal_record(const Goal &goal, ayni_bytes_v1 parameters, uint8_t *body);
// Returns false for a body that does not hold a GOAL record: a size that
// does not match, an invalid goal type id or trigger source.
bool read_goal_record(const uint8_t *body, uint32_t size, GoalRecord &out,
                      uint16_t version = goal_record_version);

// body has goal_state_record_size bytes.
void write_goal_state_record(const Goal &goal, uint8_t *body);
// Returns false for a body of another size or with unknown flags.
bool read_goal_state_record(const uint8_t *body, uint32_t size, GoalStateRecord &out);

} // namespace ayni::work

#endif // AYNI_CORE_WORK_GOAL_GOAL_RECORDS_H
