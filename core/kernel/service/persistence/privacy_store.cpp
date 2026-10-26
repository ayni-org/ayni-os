// Privacy progress is one REQUIRED record; its effect mutates only the named representation owner.
#include "kernel/service/memory/privacy_service.h"
#include "kernel/service/persistence/persistent_store.h"
#include "kernel/service/persistence/work_replay.h"
#include "work/privacy/privacy_records.h"
namespace ayni::kernel {
namespace {
bool knows(uint16_t, uint16_t version, WorkRecordContext) {
    return version == work::privacy_record_version;
}
uint32_t capacity(const KernelCapacities &c) {
    return c.memory_capacity == 0 ? 0 : 1;
}
uint32_t record_max(const KernelCapacities &) {
    return work::privacy_record_size;
}
uint32_t journal_max(const KernelCapacities &c) {
    return capacity(c) == 0 ? 0 : record_bytes(work::privacy_record_size);
}
const StoreKind kinds[] = {{work::privacy_record_type, always_required}};
bool journal(const WorkModel &model, foundation::TransitionStamp stamp,
             durability::BatchWriter &writer) {
    const auto &record = model.privacy.record();
    if (record.stamp.commit_seq != stamp.commit_seq) {
        return true;
    }
    auto *body = writer.add_record_body({work::privacy_record_type, work::privacy_record_version,
                                         durability::record_required, work::privacy_record_size});
    if (body == nullptr) {
        return false;
    }
    work::write_privacy_record(record, body);
    return true;
}
bool snapshot_next(const WorkModel &model, StoreCursor &cursor) {
    return cursor.index == 0 && model.privacy.record().phase != work::PrivacyPhase::idle;
}
StoreRecord snapshot_record(const WorkModel &, uint32_t) {
    return {work::privacy_record_version, work::privacy_record_size};
}
void snapshot_write(const WorkModel &model, uint32_t, uint8_t *body) {
    work::write_privacy_record(model.privacy.record(), body);
}
durability::BatchCheck snapshot_read(WorkModel &model, const durability::SnapshotMetadata &metadata,
                                     uint16_t, const uint8_t *body, uint32_t size) {
    work::PrivacyRecord record{};
    if (model.privacy.record().phase != work::PrivacyPhase::idle ||
        !work::read_privacy_record(body, size, record) ||
        !within_snapshot(record.stamp, metadata) ||
        (record.phase >= work::PrivacyPhase::purge &&
         record.snapshot_generation > metadata.generation)) {
        return durability::BatchCheck::corrupt;
    }
    if (model.memories.capacity() == 0) {
        return durability::BatchCheck::no_room;
    }
    // Its effect is already represented by the other complete snapshot rows; never apply twice.
    model.privacy.restore_validated(record);
    return durability::BatchCheck::ok;
}
} // namespace
const PersistentStore privacy_store{work::privacy_record_type,
                                    kinds,
                                    1,
                                    knows,
                                    StorePosition::after_work,
                                    capacity,
                                    record_max,
                                    journal_max,
                                    journal,
                                    snapshot_next,
                                    snapshot_record,
                                    snapshot_write,
                                    advance_store_index,
                                    snapshot_read};

// Source mutation requests require their exact Memory partner. Ordinary sweep steps are standalone
// transitions whose deterministic candidate/effect must equal what the retained tables imply.
durability::RecordUse WorkReplay::restore_privacy(const uint8_t *body, uint32_t size,
                                                  foundation::TransitionStamp stamp,
                                                  uint64_t boot_epoch) {
    using durability::RecordUse;
    work::PrivacyRecord record{};
    if (!work::read_privacy_record(body, size, record) ||
        record.stamp.commit_seq != stamp.commit_seq ||
        record.stamp.transition_id != stamp.transition_id || !model_.privacy.can_restore(record)) {
        return RecordUse::corrupt;
    }
    if (model_.memories.capacity() == 0) {
        return RecordUse::no_room;
    }
    if (record.request_seq == stamp.commit_seq) {
        if (pairing_.memory && pairing_.memory->privacy_required &&
            !pairing_.memory->privacy_seen) {
            pairing_.memory->privacy_seen = true;
        } else if (pairing_.context &&
                   pairing_.context->expected.kind == AYNI_INTENT_CONTEXT_FORGET &&
                   !pairing_.context->privacy_seen) {
            pairing_.context->privacy_seen = true;
        } else {
            return RecordUse::corrupt;
        }
        model_.privacy.restore_validated(record);
        return RecordUse::applied;
    }
    if (batch_seq_ == stamp.commit_seq || !pairing_.complete()) {
        return RecordUse::corrupt;
    }
    if (record.phase == work::PrivacyPhase::compact &&
        model_.privacy.record().phase == work::PrivacyPhase::files &&
        !model_.contexts.files_current()) {
        return RecordUse::corrupt; // Durable Context ACKs are model facts, not external evidence.
    }
    work::PrivacyRecord expected{};
    // Completion facts record the runtime's external barrier. Replaying never performs I/O or
    // reinterprets acknowledged files/transports; intrinsic phase transitions remain exact.
    const uint64_t generation =
        record.phase == work::PrivacyPhase::compact
            ? record.snapshot_generation - work::PrivacyCleanup::snapshot_passes
            : record.snapshot_generation;
    if (record.snapshot_generation != 0 &&
        record.snapshot_generation < work::PrivacyCleanup::snapshot_passes) {
        return RecordUse::corrupt;
    }
    if (!next_privacy_step(model_, generation, expected) ||
        expected.request_seq != record.request_seq || expected.phase != record.phase ||
        expected.cursor != record.cursor || expected.effect != record.effect ||
        expected.snapshot_generation != record.snapshot_generation ||
        !apply_privacy_step(model_, record, boot_epoch)) {
        return RecordUse::corrupt;
    }
    pairing_.privacy_progress = true;
    return RecordUse::applied;
}
} // namespace ayni::kernel
