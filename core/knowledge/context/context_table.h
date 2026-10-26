// One bounded content anchor per Agent. External files own text; this table owns version,
// complete accumulated sources and the exact outstanding file publication/deletion fact.
#ifndef AYNI_CORE_KNOWLEDGE_CONTEXT_TABLE_H
#define AYNI_CORE_KNOWLEDGE_CONTEXT_TABLE_H
#include "foundation/bounded_array.h"
#include "knowledge/memory/memory_types.h"
namespace ayni::knowledge {
// One file action is pending at a time; deletion may supersede an unfinished publication.
enum class ContextAction : uint32_t {
    none = 0,
    publish = 1,
    erase_all = 2,
    erase_round = 3,
    erase_derived = 4
};
struct ContextPending {
    ContextAction action = ContextAction::none; // None iff all companion values are zero.
    uint32_t old_revision = 0; // Deletion's before revision; zero for publication/none.
    uint64_t turn_id = 0; // Exact deleted turn, or published last_turn; zero for all/derived/none.
    uint64_t task_id = 0; // Original completed publication Task; zero for deletion/none.
};
struct ContextRecord {
    uint64_t id = 0; // Nonzero Kernel ID, stable across every revision and file generation.
    uint64_t owner_instance_id = 0; // Exactly one retained context per nonzero Agent instance.
    uint32_t revision = 1;          // Privacy barrier; advances only for accepted invalidation.
    uint64_t generation = 0; // File publication generation; zero before the first publication.
    uint64_t last_turn = 0;  // Monotone turn high-water mark; deletion never reuses turn IDs.
    ContextPending
        pending{}; // Exact file action; none only after matching durable acknowledgement.
    MemoryDependencies dependencies{};   // Complete sources, excluding this anchor's own reference.
    foundation::TransitionStamp stamp{}; // Latest complete row mutation; positive once installed.
};
class ContextTable {
  public:
    static constexpr std::size_t storage_alignment = alignof(ContextRecord);
    static std::size_t storage_size(uint32_t capacity) {
        return foundation::BoundedArray<ContextRecord>::storage_size(capacity);
    }
    // Caller arena outlives the table; capacity zero disables contexts without consuming memories.
    ContextTable(void *storage, uint32_t capacity) : rows_(storage, capacity) {}
    uint32_t size() const {
        return rows_.size();
    } // Retained anchors, at most configured Agent count.
    uint32_t capacity() const { return rows_.capacity(); } // Immutable hard bound; no eviction.
    const ContextRecord &at(uint32_t index) const {
        return rows_[index];
    } // Borrowed until mutation.
    bool find(uint64_t id, uint32_t &out) const; // Exact identity; preserves output if absent.
    bool find_owner(uint64_t owner,
                    uint32_t &out) const; // One anchor per Agent, not a display alias.
    // A context dependency names its original owner and exact privacy revision, not file
    // generation.
    bool current_reference(const MemoryDependency &ref, uint32_t &out) const;
    static bool valid(const ContextRecord &record,
                      uint32_t dependency_limit); // Shared intrinsic shape.
    // Validate before publication; existing identities cannot change owner or move backwards.
    ayni_status_t restore(const ContextRecord &record, uint32_t dependency_limit);
    // Creation was fully admitted by Kernel, including identity/owner/capacity and stamp.
    void create(uint64_t id, uint64_t owner, foundation::TransitionStamp stamp);
    // Completed Task's exact source union and next turn were checked before this publication.
    void publish(uint32_t index, uint64_t task, const MemoryDependencies &sources,
                 foundation::TransitionStamp stamp);
    // Erase mode/turn were admitted. Caller first releases the old source Grant fences.
    void erase(uint32_t index, ContextAction action, uint64_t turn,
               foundation::TransitionStamp stamp);
    // Caller proved the current privacy revision/file generation and successful external save.
    void acknowledge(uint32_t index, foundation::TransitionStamp stamp);
    bool files_current() const; // No outstanding file write/erasure for any anchor.
    void clear(); // Discard all retained metadata; no text or external file is owned here.
  private:
    foundation::BoundedArray<ContextRecord> rows_; // Sole bounded row owner, no detached indices.
};
} // namespace ayni::knowledge
#endif
