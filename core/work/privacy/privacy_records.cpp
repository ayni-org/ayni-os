#include "work/privacy/privacy_records.h"
#include "durability/batch/record_body.h"
namespace ayni::work {
void write_privacy_record(const PrivacyRecord &record, uint8_t *body) {
    durability::BodyWriter writer(body, privacy_record_size);
    writer.u64(record.request_seq);
    writer.u32(static_cast<uint32_t>(record.phase));
    writer.u32(static_cast<uint32_t>(record.effect));
    writer.u64(record.cursor);
    writer.u64(record.snapshot_generation);
    writer.u64(record.stamp.transition_id);
    writer.u64(record.stamp.commit_seq);
}
// Framing and intrinsic validation use the same typed rule as a live transition;
// replay separately checks its predecessor and the one owner effect.
bool read_privacy_record(const uint8_t *body, uint32_t size, PrivacyRecord &out) {
    if (body == nullptr || size != privacy_record_size) {
        return false;
    }
    durability::BodyReader reader(body, size);
    PrivacyRecord record{};
    record.request_seq = reader.u64();
    record.phase = static_cast<PrivacyPhase>(reader.u32());
    record.effect = static_cast<PrivacyEffect>(reader.u32());
    record.cursor = reader.u64();
    record.snapshot_generation = reader.u64();
    record.stamp = {reader.u64(), reader.u64()};
    if (!reader.ok() || !PrivacyCleanup::valid(record)) {
        return false;
    }
    out = record;
    return true;
}
} // namespace ayni::work
