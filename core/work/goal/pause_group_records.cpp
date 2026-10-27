#include "work/goal/pause_group_records.h"
#include "durability/batch/record_body.h"
namespace ayni::work {
void write_pause_group_record(uint32_t group, const PauseGroup &row, uint8_t *body) {
    durability::BodyWriter writer(body, pause_group_record_size);
    writer.u32(group);
    writer.u32(row.paused ? 1U : 0U);
    writer.u64(row.object_id);
    writer.u32(row.revision);
    writer.u32(0);
    writer.u64(row.stamp.transition_id);
    writer.u64(row.stamp.commit_seq);
    writer.u64(row.input.incarnation);
    writer.u64(row.input.through);
}
bool read_pause_group_record(const uint8_t *body, uint32_t size, PauseGroupRecord &out) {
    if (body == nullptr || size != pause_group_record_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    PauseGroupRecord value{};
    value.group = reader.u32();
    const auto paused = reader.u32();
    value.state.object_id = reader.u64();
    value.state.revision = reader.u32();
    const auto reserved = reader.u32();
    value.state.stamp = {reader.u64(), reader.u64()};
    value.state.paused = paused != 0;
    value.state.input = {reader.u64(), reader.u64()};
    if (paused > 1 || reserved != 0 || !reader.ok() || !PauseGroups::valid_group(value.group) ||
        !PauseGroups::valid_fact(value.state)) {
        return false;
    }
    out = value;
    return true;
}
} // namespace ayni::work
