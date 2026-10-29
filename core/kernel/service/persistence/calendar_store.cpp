// Calendar adapter: identical cursor/rule bytes in journal and snapshot (ADR-0061).
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"
#include "work/goal/calendar_records.h"
namespace ayni::kernel {
namespace {
bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == work::calendar_record_version;
}
uint32_t capacity(const KernelCapacities &c) {
    return c.goal_capacity;
}
uint32_t record_max(const KernelCapacities &) {
    return work::calendar_record_max_size;
}
uint32_t journal_max(const KernelCapacities &c) {
    // Group reopening can skip every member calendar in one bounded transition;
    // ordinary creation/consumption still changes only its own row (ADR-0065).
    return capacity(c) * record_bytes(work::calendar_record_max_size);
}
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    for (uint32_t index = 0; index < model.goals.size(); ++index) {
        const auto &row = model.calendars.at(index);
        if (row.goal_id == 0 || row.stamp.commit_seq != stamp.commit_seq) {
            continue;
        }
        auto *body =
            writer.add_record_body({work::calendar_record_type, work::calendar_record_version,
                                    durability::record_required, work::calendar_record_size(row)});
        if (body == nullptr) {
            return false;
        }
        work::write_calendar_record(row, body);
    }
    return true;
}
bool next(const WorkModel &model, StoreCursor &cursor) {
    while (cursor.index < model.goals.size() && model.calendars.at(cursor.index).goal_id == 0) {
        ++cursor.index;
    }
    return cursor.index < model.goals.size();
}
StoreRecord shape(const WorkModel &model, const StoreCursor &cursor) {
    const auto index = cursor.index;
    return {work::calendar_record_version, work::calendar_record_size(model.calendars.at(index))};
}
void write(const WorkModel &model, const StoreCursor &cursor, uint8_t *body) {
    const auto index = cursor.index;
    work::write_calendar_record(model.calendars.at(index), body);
}
bool restore(WorkModel &model, const work::CalendarRow &row) {
    uint32_t index = 0;
    if (!model.goals.find(row.goal_id, index)) {
        return false;
    }
    const auto &goal = model.goals.at(index);
    return work::calendar_kind(goal.trigger.kind) == row.rule.kind &&
           row.taken == goal.cursor.taken && row.stamp.commit_seq <= goal.commit_seq &&
           row.stamp.commit_seq >= goal.created.commit_seq &&
           model.calendars.restore_validated(index, row);
}
durability::BatchCheck read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                            uint16_t, const uint8_t *body, uint32_t size, uint16_t,
                            StoreReadCursor &) {
    work::CalendarRow row{};
    return work::read_calendar_record(body, size, row) && within_snapshot(row.stamp, metadata) &&
                   restore(model, row)
               ? durability::BatchCheck::ok
               : durability::BatchCheck::corrupt;
}
const StoreKind kinds[] = {{work::calendar_record_type}};
} // namespace
const PersistentStore calendar_store{work::calendar_record_type,
                                     kinds,
                                     1,
                                     knows,
                                     StorePosition::after_work,
                                     capacity,
                                     record_max,
                                     journal_max,
                                     journal,
                                     next,
                                     shape,
                                     write,
                                     advance_store_index,
                                     read};

durability::RecordUse WorkReplay::restore_calendar(const uint8_t *body, uint32_t size,
                                                   foundation::TransitionStamp stamp) {
    work::CalendarRow row{};
    const auto &pair = pairing_.reauthorization;
    return work::read_calendar_record(body, size, row) &&
                   row.stamp.commit_seq == stamp.commit_seq &&
                   row.stamp.transition_id == stamp.transition_id &&
                   (!pair || pair->goal_id == row.goal_id) && restore(model_, row)
               ? durability::RecordUse::applied
               : durability::RecordUse::corrupt;
}
} // namespace ayni::kernel
