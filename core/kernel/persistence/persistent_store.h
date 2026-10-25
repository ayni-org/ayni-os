// The persistent stores the kernel orders without reading their records
// (ADR-0051 decision 8). A store states once, beside its replay, its record
// type, the versions this build reads, its row and journal bounds, the rows
// a transition changed and how its rows enter and leave a snapshot payload.
// The journal encoder, the batch budget, header classification and the
// snapshot writer, reader and measure walk persistent_stores in log order
// instead of naming each store, so a new store adds its own file and one
// entry here. Journal restore stays in WorkReplay, which owns the pairing of
// a batch's records across stores and commands.
//
// Every version of a store's record type is REQUIRED (contracts/storage/
// records.md). The list is fixed at compile time; nothing registers at run
// time.

#ifndef AYNI_CORE_KERNEL_PERSISTENCE_PERSISTENT_STORE_H
#define AYNI_CORE_KERNEL_PERSISTENCE_PERSISTENT_STORE_H

#include "durability/batch/batch_codec.h"
#include "durability/snapshot/snapshot_codec.h"
#include "foundation/transition_stamp.h"
#include "kernel/config/kernel_config.h"
#include "kernel/intent/work_model.h"

#include <cstdint>

namespace ayni::kernel {

// The log and the snapshot payload give some versions different bodies.
enum class WorkRecordContext { journal, snapshot };

// A row's record in the snapshot payload.
struct StoreRecord {
    uint16_t version;
    uint32_t size; // body bytes, without the envelope
};

struct PersistentStore {
    uint16_t record_type; // durability/batch/record_types.h
    // Whether this build reads this version of the type in context.
    bool (*knows)(uint16_t version, WorkRecordContext context);
    // Rows these capacities retain; zero disables the store and its records.
    uint32_t (*capacity)(const KernelCapacities &capacities);
    // The largest body under these capacities.
    uint32_t (*record_max)(const KernelCapacities &capacities);
    // The most records of this type one transition writes.
    uint32_t records_per_transition;
    // Adds the records of the rows the transition stamp changed, in order;
    // false when one does not fit or the rows break the bound above, a
    // defect (AYNI_RESTRICTION_INTERNAL).
    bool (*journal)(const WorkModel &model, foundation::TransitionStamp stamp,
                    durability::BatchWriter &writer);
    // The first retained row at or after index, moving index to it; false
    // when none is left. The writer keeps the model frozen across pages.
    bool (*snapshot_next)(const WorkModel &model, uint32_t &index);
    StoreRecord (*snapshot_record)(const WorkModel &model, uint32_t index);
    // Writes the body snapshot_record(model, index) sized.
    void (*snapshot_write)(const WorkModel &model, uint32_t index, uint8_t *body);
    // Restores one payload row within the snapshot's boundary. References
    // to other stores are checked once the payload is read
    // (WorkReplay::consistent).
    durability::BatchCheck (*snapshot_read)(WorkModel &model,
                                            const durability::SnapshotMetadata &metadata,
                                            uint16_t version, const uint8_t *body, uint32_t size);
};

// Set, and not after the snapshot's boundary.
inline bool within_snapshot(foundation::TransitionStamp stamp,
                            const durability::SnapshotMetadata &metadata) {
    return stamp.transition_id != 0U && stamp.transition_id <= metadata.max_transition_id &&
           stamp.commit_seq != 0U && stamp.commit_seq <= metadata.covers_commit_seq;
}

extern const PersistentStore call_store;  // call_store.cpp
extern const PersistentStore scene_store; // scene_store.cpp

// Log and payload order: after the Tasks a call refers to, before the
// command that closes a batch.
inline constexpr const PersistentStore *persistent_stores[] = {&call_store, &scene_store};
inline constexpr uint32_t persistent_store_count =
    static_cast<uint32_t>(sizeof(persistent_stores) / sizeof(persistent_stores[0]));

// The store that writes type and its place in persistent_stores; false for
// a type the kernel's own tables write.
inline bool find_persistent_store(uint16_t type, uint32_t &out_rank) {
    for (uint32_t rank = 0; rank < persistent_store_count; ++rank) {
        if (persistent_stores[rank]->record_type == type) {
            out_rank = rank;
            return true;
        }
    }
    return false;
}

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_PERSISTENCE_PERSISTENT_STORE_H
