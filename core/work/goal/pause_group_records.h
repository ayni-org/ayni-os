// Pause preferences, records.md type28/v1; logical input frontiers preserve reopening across writer
// reload.
#ifndef AYNI_CORE_WORK_GOAL_PAUSE_GROUP_RECORDS_H
#define AYNI_CORE_WORK_GOAL_PAUSE_GROUP_RECORDS_H
#include "durability/batch/record_types.h"
#include "work/goal/pause_groups.h"
namespace ayni::work {
constexpr uint16_t pause_group_record_type = durability::record_type::pause_group;
constexpr uint16_t pause_group_record_version = 1;
constexpr uint32_t pause_group_record_size = 56; // records.md scalar row plus transition stamp.
// Complete transport fact; number selects one fixed slot, state owns the durable preference.
struct PauseGroupRecord {
    uint32_t group = 0; // Valid nonzero group number; zero only before decoding.
    PauseGroup state{}; // Complete durable preference, including logical reopening frontier.
};
// Complete state writer; caller supplies exactly record_size bytes.
void write_pause_group_record(uint32_t group, const PauseGroup &row, uint8_t *body);
// Framing and shared intrinsic fact validation, without mutating a table.
bool read_pause_group_record(const uint8_t *body, uint32_t size, PauseGroupRecord &out);
} // namespace ayni::work
#endif
