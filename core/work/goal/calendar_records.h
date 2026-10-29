// Calendar row layout has one reader/writer (contracts/storage/records.md, type 27).
#ifndef AYNI_CORE_WORK_GOAL_CALENDAR_RECORDS_H
#define AYNI_CORE_WORK_GOAL_CALENDAR_RECORDS_H
#include "durability/batch/record_types.h"
#include "foundation/record_body.h"
#include "work/goal/calendar_table.h"
namespace ayni::work {
inline constexpr uint16_t calendar_record_type = durability::record_type::calendar;
inline constexpr uint16_t calendar_record_version = 1;
inline constexpr uint32_t calendar_rule_fixed_size = 20;   // kind/version/local anchor/TZ length.
inline constexpr uint32_t calendar_record_fixed_size = 64; // rule + Goal/stamp/cursor fields.
inline constexpr uint32_t calendar_record_max_size =
    calendar_record_fixed_size + AYNI_CALENDAR_TZ_MAX_BYTES;
uint32_t
calendar_rule_size(const CalendarRule &rule); // Zero for absence; exact used TZ bytes otherwise.
void write_calendar_rule(const CalendarRule &rule, foundation::BodyWriter &writer);
bool read_calendar_rule(foundation::BodyReader &reader, CalendarRule &out);
uint32_t calendar_record_size(const CalendarRow &row);
void write_calendar_record(const CalendarRow &row, uint8_t *body);
bool read_calendar_record(const uint8_t *body, uint32_t size, CalendarRow &out);
} // namespace ayni::work
#endif
