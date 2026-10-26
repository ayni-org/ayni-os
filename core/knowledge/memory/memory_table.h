// Owns bounded memory facts, byte storage, revision rules and dependency graph traversal.
// Kernel supplies authenticated actors, Grant/context checks and committed transition stamps.
#ifndef AYNI_CORE_KNOWLEDGE_MEMORY_TABLE_H
#define AYNI_CORE_KNOWLEDGE_MEMORY_TABLE_H
#include "foundation/bounded_array.h"
#include "knowledge/memory/memory_types.h"
namespace ayni::knowledge {

// Kernel-internal liveness predicate: checks exact Grant refs and context anchors, no user
// callback.
struct MemoryDependencyCheck {
    const void *context; // Borrowed owner context, alive through the synchronous traversal.
    bool (*current)(const void *, const MemoryDependency &); // False denies the entire graph.
};

// A validated write into the unchanged table; caller consumes it before another mutation.
struct MemoryWritePlan {
    uint32_t index;    // Existing row or the append position; never a reclaimed live record.
    uint32_t revision; // Current + 1 for edits, memory_initial_revision for creation.
};

class MemoryTable {
  public:
    // All retained facts and body strides live in one caller-owned aligned arena.
    static constexpr std::size_t storage_alignment = alignof(MemoryRecord);
    static std::size_t storage_size(MemoryLimits limits);
    // Null only for disabled storage; the caller retains the arena throughout the table lifetime.
    MemoryTable(void *storage, MemoryLimits limits);
    // Retained count includes tombstones until physical cleanup removes their audit row.
    uint32_t size() const { return rows_.size(); }
    // Configured capacity is a hard limit, not an eviction target.
    uint32_t capacity() const { return limits_.capacity; }
    // Scalar configuration used by codecs/Kernel admission; never changes in this boot.
    MemoryLimits limits() const { return limits_; }
    // Missing/zero IDs preserve out_index; IDs never mean moving row indices.
    bool find(uint64_t id, uint32_t &out_index) const;
    // Read-only facts and body; index < size(), invalidated by any mutation.
    const MemoryRecord &at(uint32_t index) const { return rows_[index]; }
    // Borrow exactly body_size UTF-8 bytes; scrubbed rows return an empty view.
    ayni_string_v1 body(uint32_t index) const;
    // Intrinsic form, namespace immutability, CAS and capacity checks; no mutation on failure.
    // A zero target and expected revision create; existing targets require a positive revision.
    ayni_status_t check_write(uint64_t target, uint32_t expected_revision, const MemoryValue &value,
                              MemoryWritePlan &out) const;
    // Copy a fully admitted value with a fresh allocated ID or the unchanged target identity.
    // Input views must not alias table storage; all fallible checks precede this call.
    void install(const MemoryWritePlan &plan, uint64_t id, const MemoryValue &value,
                 foundation::TransitionStamp stamp);
    // Shared CAS for confirmation/deletion. Never permits edits of a deleted record.
    ayni_status_t check_live(uint64_t id, uint32_t expected_revision, uint32_t &out_index) const;
    // Owner-confirmed state retains source and lineage; callers already checked current deps/CAS.
    void confirm(uint32_t index, foundation::TransitionStamp stamp);
    // Immediate use barrier; body persists only until the bounded scrub step.
    void tombstone(uint32_t index, foundation::TransitionStamp stamp);
    // Erase body/labels/lineage after the barrier, retaining only necessary identity/source facts.
    void scrub(uint32_t index, foundation::TransitionStamp stamp);
    // Remove an already scrubbed row once its deletion facts can no longer be lost by compaction.
    void remove_scrubbed(uint32_t index);
    // Validate the entire direct/transitive graph with bounded local storage and no recursion.
    // forbidden_id rejects prospective self/cycles during edit; zero means no prospective edit.
    bool dependencies_current(const MemoryDependencies &dependencies, uint64_t durable_seq,
                              MemoryDependencyCheck external, uint64_t forbidden_id = 0) const;
    // Only a live, durable row with a completely usable graph can enter a new context.
    bool current(uint32_t index, uint64_t durable_seq, MemoryDependencyCheck external) const;
    // Exact tag/keyword or body substring. Access/current checks belong before body exposure.
    bool matches(uint32_t index, ayni_string_v1 keyword, ayni_string_v1 tag) const;
    // Shared record/body validation and capacity classification, used for journal and snapshots.
    ayni_status_t restore(const MemoryRecord &record, ayni_string_v1 body);
    // Drop all recovered facts and erase the whole bounded body arena; no authorization state here.
    void clear();

  private:
    // Row-local storage, with index below capacity; no external caller receives mutable bytes.
    char *body_storage(uint32_t index) const;
    foundation::BoundedArray<MemoryRecord>
        rows_;            // Sole retained metadata, no sparse/sentinel rows.
    char *bytes_;         // Body stride region; null only when capacity is zero.
    MemoryLimits limits_; // Validated immutable bounds for rows, bodies and direct lineage.
};
} // namespace ayni::knowledge
#endif
