// The GOAL and GOAL_STATE records (contracts/storage/records.md, ADR-0026).
// A transition that creates a goal writes its GOAL record, what the goal was
// created with, and every transition that changes a goal, its creation
// included, writes its GOAL_STATE record, what the change left. The stamp of
// both is their batch's. Times are on one boot's clock, so the records keep
// durations and whether a time is set, never the time.

#ifndef AYNI_CORE_WORK_GOAL_GOAL_RECORDS_H
#define AYNI_CORE_WORK_GOAL_GOAL_RECORDS_H

#include "durability/batch/record_types.h"
#include "work/goal/goal_table.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::work {

constexpr uint16_t goal_record_type = durability::record_type::goal;
constexpr uint16_t goal_record_version = 1;
// Snapshot adds retained creation/current stamps; it cannot be read as a journal row.
constexpr uint16_t goal_snapshot_record_version = 2;
constexpr uint32_t goal_options_size = 2 * sizeof(uint32_t); // authority + provenance markers.
constexpr uint32_t goal_group_tail_size = sizeof(uint32_t);  // Current immutable pause group.
constexpr uint32_t goal_work_profile_size =
    16; // u32 class + u32 recovery + u64 period floor, schema 11 records.md.
constexpr uint16_t goal_state_record_type = durability::record_type::goal_state;
constexpr uint16_t goal_state_record_version = 1;
// Cross-record proof for this state transition, explicit in schema 4 rather than
// encoded in a historical layout version. Snapshot state uses state.
enum class GoalRecordChange : uint32_t {
    state = 1,
    authority = 2,
    cleanup = 3,
    authority_pause = 4,
    reauthorization = 5,
};
// Creation partners owned by the surrounding transition; absence is explicit.
struct GoalRecordOptions {
    bool authority = false;  // A matching authority row is required when true.
    bool provenance = false; // A matching source footer is required; implies authority.
};
// A GOAL record without its goal type id, trigger source and parameters.
constexpr uint32_t goal_record_fixed_size =
    136 + catalog::tool_policy_header_bytes; // Complete schema 11 creation, records.md.
constexpr uint32_t goal_state_record_size =
    48 + sizeof(uint32_t); // Full state then GoalRecordChange.

// What a GOAL record holds. goal.parameters points into the body, and
// goal.goal_type_index and goal.created_us are left 0.
struct GoalRecord {
    uint64_t goal_id;
    uint64_t replaced_goal_id;
    uint32_t depth;
    NewGoal goal;
    bool authority_required = false; // True requires a paired ceiling; no Goal field copy.
    bool provenance_required =
        false; // True requires source metadata before use/replay publication.
};

struct GoalStateRecord {
    uint64_t goal_id;
    GoalState state;
    GoalRecordChange change =
        GoalRecordChange::state; // Required batch proof; no cached Goal field.
};

uint32_t goal_record_size(const Goal &goal);
// Current complete body; parameters are retained bytes, options name required partners.
void write_goal_record(const Goal &goal, ayni_bytes_v1 parameters, uint8_t *body,
                       GoalRecordOptions options);
// Returns false for a body that does not hold a GOAL record: a size that
// does not match, an invalid goal type id or trigger source.
bool read_goal_record(const uint8_t *body, uint32_t size, GoalRecord &out);

// body has goal_state_record_size bytes.
void write_goal_state_record(const Goal &goal, uint8_t *body, GoalRecordChange change);
// Returns false for a body of another size or with unknown flags.
bool read_goal_state_record(const uint8_t *body, uint32_t size, GoalStateRecord &out);

} // namespace ayni::work

#endif // AYNI_CORE_WORK_GOAL_GOAL_RECORDS_H
