// Work owns the complete accumulated content dependencies of Goals and Tasks (ADR-0060).
// Goal indices and Task stable slots belong to their respective tables; Kernel supplies them.
#ifndef AYNI_CORE_WORK_PROVENANCE_TABLE_H
#define AYNI_CORE_WORK_PROVENANCE_TABLE_H
#include "knowledge/memory/memory_types.h"
#include <cstddef>
namespace ayni::work {
// Record kind selects its work owner's independently bounded slot region.
enum class ProvenanceKind : uint32_t { goal = 1, task = 2 };
// Content use is independent of work/action outcome; erasure never fabricates a failed action.
enum class ContentState : uint32_t {
    available = 1,
    erased = 2,
    awaiting_record = 3 // Replay-only expectation; never serialized or usable.
};
// One retained work identity's content provenance; zero ID means an unused slot.
struct ContextOrigin {
    uint64_t id = 0;         // Primary context anchor; zero means independent Goal input.
    uint64_t generation = 0; // Exact input file generation, zero is valid for an empty new context.
    // Absent input has no generation; generation zero is also a valid present file.
    bool valid() const { return id != 0 || generation == 0; }
};
struct ProvenanceRecord {
    uint64_t id = 0;                                // Exact Goal/Task identity; no record iff zero.
    ProvenanceKind kind = ProvenanceKind::goal;     // Determines the work owner and slot region.
    ContentState content = ContentState::available; // Erased is terminal for this exact work ID.
    knowledge::MemoryDependencies dependencies{};   // Complete accumulated set; never truncated.
    foundation::TransitionStamp stamp{}; // Last accepted union/erasure, positive iff ID is nonzero.
    ContextOrigin origin{}; // Goal-only file publication basis; Task references its retained Goal.
};
// Owns metadata only; Goal/Task/Call owners erase their own body buffers.
class ProvenanceTable {
  public:
    // Fixed sizes by retained Goal count and Task stable-slot count; zero disables both.
    struct Limits {
        uint32_t goals; // Goal-index slots, zero iff disabled.
        uint32_t tasks; // Task stable slots, zero iff disabled.
    };
    static constexpr std::size_t storage_alignment = alignof(ProvenanceRecord);
    // Includes every slot, with no heap storage or separate dynamic index.
    static std::size_t storage_size(Limits limits);
    // Caller arena outlives the table; null only when both counts are zero.
    ProvenanceTable(void *storage, Limits limits);
    // Total allocated slots, including unused identities.
    uint32_t capacity() const { return limits_.goals + limits_.tasks; }
    // Read-only flat traversal for cleanup/persistence; index < capacity().
    const ProvenanceRecord &at(uint32_t index) const { return records_[index]; }
    // Missing exact identity returns null; a replaced slot cannot inherit old provenance.
    const ProvenanceRecord *find(ProvenanceKind kind, uint32_t slot, uint64_t id) const;
    // Snapshot REQUIRED content marker reserves this exact identity until its source row arrives.
    bool expect(ProvenanceKind kind, uint32_t slot, uint64_t id);
    // No unresolved required metadata may cross the recovery publication boundary.
    bool complete() const;
    // Consume fully validated, complete dependencies in the same transition as the owner.
    void assign(ProvenanceKind kind, uint32_t slot, uint64_t id,
                const knowledge::MemoryDependencies &dependencies,
                foundation::TransitionStamp stamp, ContextOrigin origin = {});
    // Irreversibly erase content and unlink its source set; Kernel releases exact Grant fences
    // before this same-transition mutation. A Task inheriting only Goal sources may have no row
    // yet.
    void erase(ProvenanceKind kind, uint32_t slot, uint64_t id, foundation::TransitionStamp stamp);
    // A work identity gives way only with its owner's replacement transition.
    void clear(ProvenanceKind kind, uint32_t slot);
    // Discard recovered metadata; initialization and reload share one full clear.
    void clear();
    // Restore typed row into its work owner's validated slot; no body pointers are retained.
    ayni_status_t restore(uint32_t slot, const ProvenanceRecord &record, uint32_t dependency_limit);

  private:
    // One owner-defined slot mapping used by lookup, mutation and restore.
    uint32_t index(ProvenanceKind kind, uint32_t slot) const;
    bool valid_slot(ProvenanceKind kind, uint32_t slot) const;
    ProvenanceRecord *records_; // Sole arena-owned rows; null only for disabled table.
    Limits limits_; // Immutable region sizes; sum fits configured Kernel work capacities.
};
} // namespace ayni::work
#endif
