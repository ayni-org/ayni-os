#include "foundation/little_endian.h"
#include "foundation/record_body.h"
#include "kernel/service/persistence/persistent_store.h"
#include "work/agent/agent_records.h"
namespace ayni::kernel {
namespace {
constexpr uint32_t stamp_size = snapshot_stamp_size;
constexpr uint32_t instance_size = work::instance_record_size + stamp_size;

durability::RecordHeader header(uint16_t type, uint16_t version, uint32_t length) {
    return {type, version, durability::record_required, length};
}
// Owns this family's encoding in the original schema 4 journal order.
class Journal {
  public:
    Journal(const WorkModel &model, foundation::TransitionStamp stamp,
            durability::BatchWriter &writer)
        : model_(model), stamp_(stamp), writer_(writer) {}
    bool encode() { return instances(); }

  private:
    uint8_t *body(uint16_t type, uint16_t version, uint32_t length) {
        uint8_t *at = writer_.add_record_body(header(type, version, length));
        ok_ = ok_ && at != nullptr;
        return at;
    }
    bool instances() {
        for (uint32_t index = 0; index < model_.agents.size(); ++index) {
            const work::AgentInstance &instance = model_.agents.at(index);
            if (instance.stamp.commit_seq != stamp_.commit_seq) {
                continue;
            }
            uint8_t *at = body(work::instance_record_type, work::instance_record_version,
                               work::instance_record_size);
            if (at != nullptr) {
                work::write_instance_record(instance, at);
            }
        }
        return ok_;
    }
    const WorkModel &model_;
    foundation::TransitionStamp stamp_;
    durability::BatchWriter &writer_;
    bool ok_ = true;
};
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    return Journal(model, stamp, writer).encode();
}
// INSTANCE has one current body; schema 4 requires its REQUIRED marker.
bool knows(uint16_t, uint16_t version, WorkRecordContext context) {
    return version == (context == WorkRecordContext::journal
                           ? work::instance_record_version
                           : work::instance_snapshot_record_version);
}
uint32_t capacity(const KernelCapacities &c) {
    return c.agent_capacity;
}
uint32_t record_max(const KernelCapacities &) {
    return instance_size;
}
uint32_t journal_max(const KernelCapacities &c) {
    return c.agent_capacity * record_bytes(work::instance_record_size);
}
// Never-started registrations have no durable identity and emit no row.
bool next(const WorkModel &model, StoreCursor &cursor) {
    while (cursor.index < model.agents.size() && model.agents.at(cursor.index).instance_id == 0) {
        ++cursor.index;
    }
    return cursor.index < model.agents.size();
}
StoreRecord shape(const WorkModel &, const StoreCursor &) {
    return {work::instance_snapshot_record_version, instance_size};
}
// Snapshot identity carries the last lifecycle stamp after the journal body.
void write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto &row = model.agents.at(cursor.index);
    work::write_instance_record(row, body);
    write_snapshot_stamp(body + work::instance_record_size,
                         {row.stamp.transition_id, row.stamp.commit_seq});
}
// Restore one bounded identity only once, under the immutable snapshot boundary.
durability::BatchCheck instance(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                StoreReadCursor &, const uint8_t *body, uint32_t size) {
    work::InstanceRecord record{};
    if (size != instance_size ||
        !work::read_instance_record(body, work::instance_record_size, record)) {
        return durability::BatchCheck::corrupt;
    }
    const foundation::TransitionStamp stamp =
        read_snapshot_stamp(body + work::instance_record_size);
    if (!within_snapshot(stamp, metadata) ||
        model.agents.find_instance(record.instance_id) != nullptr) {
        return durability::BatchCheck::corrupt;
    }
    return snapshot_restore_check(model.agents.restore(record.agent_id, record.instance_id,
                                                       record.revision, record.lifecycle, stamp));
}
durability::BatchCheck read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                            uint16_t, const uint8_t *body, uint32_t size, uint16_t,
                            StoreReadCursor &cursor) {
    return instance(model, metadata, cursor, body, size);
}
const StoreKind kinds[] = {{work::instance_record_type}};

} // namespace
const PersistentStore agent_store{work::instance_record_type,
                                  kinds,
                                  static_cast<uint32_t>(sizeof(kinds) / sizeof(kinds[0])),
                                  knows,
                                  StorePosition::instances,
                                  capacity,
                                  record_max,
                                  journal_max,
                                  journal,
                                  next,
                                  shape,
                                  write,
                                  advance_store_index,
                                  read};
} // namespace ayni::kernel
