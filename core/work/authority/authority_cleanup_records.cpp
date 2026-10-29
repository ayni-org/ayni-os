#include "work/authority/authority_cleanup_records.h"

#include "foundation/record_body.h"

namespace ayni::work {

void write_authority_cleanup_record(const AuthorityCleanupRecord &record, uint8_t *body) {
    foundation::BodyWriter writer(body, authority_cleanup_record_size);
    writer.u64(record.stamp.transition_id);
    writer.u64(record.stamp.commit_seq);
    writer.u32(static_cast<uint32_t>(record.phase));
    writer.u64(record.after_goal_id);
    writer.u64(record.through_goal_id);
    writer.u64(record.requested_through);
}

bool read_authority_cleanup_record(const uint8_t *body, uint32_t size,
                                   AuthorityCleanupRecord &out) {
    if (body == nullptr || size != authority_cleanup_record_size) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    AuthorityCleanupRecord record{};
    record.stamp = {reader.u64(), reader.u64()};
    record.phase = static_cast<AuthorityScanPhase>(reader.u32());
    record.after_goal_id = reader.u64();
    record.through_goal_id = reader.u64();
    record.requested_through = reader.u64();
    if (!reader.ok() || !valid_authority_cleanup_record(record)) {
        return false;
    }
    out = record;
    return true;
}

} // namespace ayni::work
