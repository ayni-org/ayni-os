// REQUIRED Task authority layout, contracts/storage/records.md type 18/v1.
// One codec serves journal/snapshot; replay proves owning Goal/Task facts.
#ifndef AYNI_CORE_WORK_TASK_TASK_AUTHORITY_RECORDS_H
#define AYNI_CORE_WORK_TASK_TASK_AUTHORITY_RECORDS_H
#include "durability/batch/record_types.h"
#include "work/task/task_authority.h"
namespace ayni::work {

constexpr uint16_t task_authority_record_type = durability::record_type::task_authority;
constexpr uint16_t task_authority_record_version = 1;
// Five u64 identity/stamp words, then phase/count u32; references are u64/u32.
constexpr uint32_t task_authority_record_fixed_size = 48;
constexpr uint32_t task_authority_reference_size = 12;
constexpr uint32_t task_authority_phase_at = 40;
constexpr uint32_t task_authority_count_at = 44;
// Counts use configured Goal requirements; no C++ struct layout is serialized.
constexpr uint32_t task_authority_record_max(uint32_t requirements) {
    return task_authority_record_fixed_size + requirements * task_authority_reference_size;
}

// Requires validated facts and exactly record_max(record.count) writable bytes.
void write_task_authority_record(const TaskAuthorityRecord &record, uint8_t *body);
// Failure preserves out; shared shape validation occurs once after parsing.
bool read_task_authority_record(const uint8_t *body, uint32_t size, TaskAuthorityRecord &out);
} // namespace ayni::work

#endif // AYNI_CORE_WORK_TASK_TASK_AUTHORITY_RECORDS_H
