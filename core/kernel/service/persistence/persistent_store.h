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
// REQUIRED versions are stated beside each family; legacy command v1 remains
// optional (contracts/storage/records.md). Nothing registers at run time.

#ifndef AYNI_CORE_KERNEL_PERSISTENCE_PERSISTENT_STORE_H
#define AYNI_CORE_KERNEL_PERSISTENCE_PERSISTENT_STORE_H

#include "durability/batch/batch_codec.h"
#include "durability/snapshot/snapshot_codec.h"
#include "foundation/transition_stamp.h"
#include "kernel/config/kernel_config.h"
#include "kernel/service/model/work_model.h"

#include <cstdint>

namespace ayni::kernel {

// The log and the snapshot payload give some versions different bodies.
enum class WorkRecordContext { journal, snapshot };

// A row's record in the snapshot payload.
struct StoreRecord {
    uint16_t version; // Supported body version for this complete retained row.
    uint32_t size;    // body bytes, without the envelope
};

// Store-owned snapshot traversal. index is a retained row; order is zero initially,
// then a Grant depth (edges) or the last emitted command commit sequence. Other stores
// use index alone and keep order zero. It never survives one frozen snapshot walk.
struct StoreCursor {
    uint32_t index = 0; // Row index; snapshot_next selects it, snapshot_advance consumes it.
    uint64_t order = 0; // Store-specific ordering frontier described above, not a persisted value.
};

// Existing storage order: Grant facts precede work; calls/Scenes follow Tasks;
// command snapshots follow cleanup, while journal commands close before its footer.
enum class StorePosition { before_work, after_work, after_cleanup };

// One registered record family; required_from preserves optional legacy command v1.
struct StoreKind {
    uint16_t type;          // records.md type identity, never zero.
    uint16_t required_from; // First REQUIRED version; older versions keep their existing rule.
};

// Owns one compile-time storage adapter; WorkReplay retains cross-store journal proofs.
struct PersistentStore {
    uint16_t record_type;   // Snapshot row type; journal may write several declared kinds.
    const StoreKind *kinds; // Immutable compile-time family; kind_count entries.
    uint32_t kind_count;    // Positive family length; no runtime registration.
    // Whether this build reads this type/version in the given storage context.
    bool (*knows)(uint16_t type, uint16_t version, WorkRecordContext context);
    StorePosition position; // Placement relative to work/cleanup, preserving schema 2 order.
    // Rows these capacities retain; zero disables snapshot rows.
    uint32_t (*capacity)(const KernelCapacities &capacities);
    // Largest snapshot body, excluding the envelope.
    uint32_t (*record_max)(const KernelCapacities &capacities);
    // Complete journal contribution, including envelopes and bounded startup sweeps.
    uint32_t (*journal_max)(const KernelCapacities &capacities);
    // Encode this transition's changed facts, preserving this store's internal order.
    // False means a record does not fit or violates the declared bound: an internal defect.
    bool (*journal)(const WorkModel &model, foundation::TransitionStamp stamp,
                    durability::BatchWriter &writer);
    // Select without consuming; repeated selection of a frozen model is idempotent.
    bool (*snapshot_next)(const WorkModel &model, StoreCursor &cursor);
    StoreRecord (*snapshot_record)(const WorkModel &model, uint32_t index); // Selected row shape.
    // Write the selected body into already measured caller storage.
    void (*snapshot_write)(const WorkModel &model, uint32_t index, uint8_t *body);
    // Consume only after a whole record fits/is written, or after measuring it.
    void (*snapshot_advance)(const WorkModel &model, StoreCursor &cursor);
    // Restore one independently bounded row; WorkReplay keeps cross-record batch pairing.
    // References across stores are proved after the payload by WorkReplay::consistent.
    durability::BatchCheck (*snapshot_read)(WorkModel &model,
                                            const durability::SnapshotMetadata &metadata,
                                            uint16_t version, const uint8_t *body, uint32_t size);
};

// Known non-command families require the marker even for an unknown version zero.
inline constexpr uint16_t always_required = 0;
// Count one record envelope without repeating its layout in store adapters.
inline constexpr uint32_t record_bytes(uint32_t body_size) {
    return static_cast<uint32_t>(durability::record_header_size) + body_size;
}
// The common row-order traversal consumes one retained table index.
inline void advance_store_index(const WorkModel &, StoreCursor &cursor) {
    ++cursor.index;
}

// Restore classifications are shared by all row adapters and the remaining work reader.
template <typename Restore> durability::BatchCheck snapshot_restore_check(Restore value) {
    switch (value) {
    case Restore::restored:
        return durability::BatchCheck::ok;
    case Restore::no_room:
        return durability::BatchCheck::no_room;
    case Restore::corrupt:
        return durability::BatchCheck::corrupt;
    }
    return durability::BatchCheck::corrupt;
}

// C-STORE-015/017: retained parents precede children despite terminal slot reuse.
// The caller owns the bounded scan cursor and keeps the model frozen.
template <typename Table, typename Depth>
bool next_snapshot_at_depth(const Table &table, uint32_t max_depth, Depth &depth, uint32_t &index) {
    for (;;) {
        while (index < table.size() && table.at(index).depth != depth) {
            ++index;
        }
        if (index < table.size()) {
            return true;
        }
        if (depth == max_depth) {
            return false;
        }
        ++depth;
        index = 0;
    }
}

// Set, and not after the snapshot's boundary.
inline bool within_snapshot(foundation::TransitionStamp stamp,
                            const durability::SnapshotMetadata &metadata) {
    return stamp.transition_id != 0U && stamp.transition_id <= metadata.max_transition_id &&
           stamp.commit_seq != 0U && stamp.commit_seq <= metadata.covers_commit_seq;
}

extern const PersistentStore grant_store;      // Grant family and parent-first snapshots.
extern const PersistentStore call_store;       // Calls after their Task facts.
extern const PersistentStore scene_store;      // Scene preferences after calls.
extern const PersistentStore provenance_store; // Work source sets follow original work identities.
extern const PersistentStore memory_store;     // Memory rows after their namespace identities.
extern const PersistentStore privacy_store;    // Source request or one explicit cleanup effect.
extern const PersistentStore context_store;    // Agent file anchors and exact pending file actions.
extern const PersistentStore command_store;    // Exact owner command history closes each walk.

// Compile-time storage order; each descriptor owns the types, bounds and row traversal.
inline constexpr const PersistentStore *persistent_stores[] = {
    &grant_store,  &call_store,       &scene_store,   &context_store,
    &memory_store, &provenance_store, &privacy_store, &command_store};
inline constexpr uint32_t persistent_store_count =
    static_cast<uint32_t>(sizeof(persistent_stores) / sizeof(persistent_stores[0]));

// The store that writes type and its place in persistent_stores; false for
// a type the kernel's own tables write.
inline bool find_persistent_store(uint16_t type, uint32_t &out_rank) {
    for (uint32_t rank = 0; rank < persistent_store_count; ++rank) {
        const auto &store = *persistent_stores[rank];
        for (uint32_t kind = 0; kind < store.kind_count; ++kind) {
            if (store.kinds[kind].type == type) {
                out_rank = rank;
                return true;
            }
        }
    }
    return false;
}
// Unknown versions of a known family retain their historical REQUIRED boundary.
inline bool store_requires_marker(const PersistentStore &store, uint16_t type, uint16_t version) {
    for (uint32_t kind = 0; kind < store.kind_count; ++kind) {
        if (store.kinds[kind].type == type) {
            return version >= store.kinds[kind].required_from;
        }
    }
    return false;
}

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_PERSISTENCE_PERSISTENT_STORE_H
