#include "work/goal/calendar_records.h"
#include <cstring>
namespace ayni::work {
uint32_t calendar_rule_size(const CalendarRule &rule) {
    return rule.kind == 0 ? 0 : calendar_rule_fixed_size + rule.timezone_size;
}
void write_calendar_rule(const CalendarRule &rule, foundation::BodyWriter &writer) {
    writer.u32(rule.kind);
    writer.u32(rule.version);
    writer.u64(static_cast<uint64_t>(rule.first_local_s));
    writer.bytes(rule.timezone, rule.timezone_size);
}
bool read_calendar_rule(foundation::BodyReader &reader, CalendarRule &out) {
    ayni_calendar_rule_v1 input{};
    input.struct_size = sizeof(input);
    input.kind = reader.u32();
    input.rule_version = reader.u32();
    const auto local = reader.u64();
    uint32_t size = 0;
    const auto *tz = reader.bytes(AYNI_CALENDAR_TZ_MAX_BYTES, size);
    if (local > static_cast<uint64_t>(AYNI_CALENDAR_SECONDS_MAX) || tz == nullptr) {
        return false;
    }
    input.first_local_s = static_cast<int64_t>(local);
    input.timezone_size = size;
    std::memcpy(input.timezone, tz, size);
    return decode_calendar_rule(input, out) == AYNI_OK;
}
uint32_t calendar_record_size(const CalendarRow &row) {
    return calendar_record_fixed_size + row.rule.timezone_size;
}
void write_calendar_record(const CalendarRow &row, uint8_t *body) {
    foundation::BodyWriter writer(body, calendar_record_size(row));
    writer.u64(row.goal_id);
    writer.u64(row.stamp.transition_id);
    writer.u64(row.stamp.commit_seq);
    writer.u64(static_cast<uint64_t>(row.last_local_s));
    writer.u64(static_cast<uint64_t>(row.last_utc_s));
    writer.u32(row.taken);
    write_calendar_rule(row.rule, writer);
}
bool read_calendar_record(const uint8_t *body, uint32_t size, CalendarRow &out) {
    foundation::BodyReader reader(body, size);
    CalendarRow row{};
    row.goal_id = reader.u64();
    row.stamp = {reader.u64(), reader.u64()};
    const auto local = reader.u64();
    const auto utc = reader.u64();
    row.taken = reader.u32();
    if ((local > static_cast<uint64_t>(AYNI_CALENDAR_SECONDS_MAX) && local != UINT64_MAX) ||
        (utc > static_cast<uint64_t>(AYNI_CALENDAR_SECONDS_MAX) && utc != UINT64_MAX) ||
        !read_calendar_rule(reader, row.rule) || !reader.ok()) {
        return false;
    }
    row.last_local_s = local == UINT64_MAX ? AYNI_CALENDAR_TIME_NONE : static_cast<int64_t>(local);
    row.last_utc_s = utc == UINT64_MAX ? AYNI_CALENDAR_TIME_NONE : static_cast<int64_t>(utc);
    if (!valid_calendar_cursor(row)) {
        return false;
    }
    out = row;
    return true;
}
} // namespace ayni::work
