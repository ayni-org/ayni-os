// Task-owned authorization snapshots (ADR-0038). Slots follow TaskTable::slot,
// not its movable row index. Goal owns immutable ceilings and Scope bytes.
#ifndef AYNI_CORE_WORK_TASK_TASK_AUTHORITY_H
#define AYNI_CORE_WORK_TASK_TASK_AUTHORITY_H
#include "work/authority/authority_facts.h"

#include <array>
#include <ayni/ayni.h>

#include <cstddef>
#include <cstdint>

namespace ayni::work {

// Durable authorization phase, independent of user/policy/recovery holds.
enum class TaskAuthorityPhase : uint32_t { authorized = 1, paused = 2 };

// Complete admission facts; references[0,count) match Goal requirement order.
struct TaskAuthorityRecord {
    uint64_t task_id = 0;                // Exact nonzero Task ID, never a slot index.
    BindingReference ceiling;            // Owning Goal and binding inherited at admission.
    foundation::TransitionStamp stamp{}; // Positive pair of the last authorization update.
    TaskAuthorityPhase phase = TaskAuthorityPhase::authorized; // No proposed executable Task.
    uint32_t count = 0; // References, bounded by Goal's configured requirement capacity.
    std::array<AuthorityReference, AYNI_GOAL_REQUIREMENT_CAPACITY_MAX> references{};
};

// One shape validator for bytes and struct admission, without live Grant lookup.
bool valid_task_authority_record(const TaskAuthorityRecord &record);
// Owns one fixed slot per Task storage slot and bounded reference regions.
class TaskAuthorityTable {
  public:
    // Arena dimensions; Kernel validates absolute bounds before construction.
    struct Limits {
        uint32_t tasks;        // Positive Task capacity, matching TaskTable.
        uint32_t requirements; // Positive references per Task, matching Goal requirement capacity.
    };
    // Complete arena bytes/alignment; no Scope storage or allocation.
    static constexpr std::size_t storage_alignment = alignof(uint64_t);
    static std::size_t storage_size(Limits limits);
    // Caller owns aligned storage_size(limits) bytes for the table's lifetime.
    TaskAuthorityTable(void *storage, Limits limits);
    TaskAuthorityTable(const TaskAuthorityTable &) = delete;
    TaskAuthorityTable &operator=(const TaskAuthorityTable &) = delete;
    // Requires shared-validated unchanged facts; capacity refusal changes no slot.
    ayni_status_t install_validated(uint32_t slot, const TaskAuthorityRecord &record);
    // Same identity/count, monotone stamp/binding; replacement requires clear().
    // Rebinding is proved by the enclosing replay transaction, not this table.
    ayni_status_t restore_validated(uint32_t slot, const TaskAuthorityRecord &record);
    // Copies complete facts for the exact Task only; failure preserves out.
    bool read(uint32_t slot, uint64_t task_id, TaskAuthorityRecord &out) const;
    // Presence is independent of count: an admitted zero-needs Task has a row.
    bool contains(uint32_t slot, uint64_t task_id) const;
    // Clears this slot; out-of-range slots change nothing.
    void clear(uint32_t slot);
    // Recovery reset; caller has already stopped consumption.
    void clear_all();

  private:
    // task_id is explicit presence: 0 means all other members are defaults.
    struct Slot {
        uint64_t task_id = 0;                // Exact Task, or 0 when absent.
        BindingReference ceiling;            // Goal and admitted binding sequence.
        foundation::TransitionStamp stamp{}; // Last update, zero only when absent.
        TaskAuthorityPhase phase = TaskAuthorityPhase::authorized; // Meaningful when present.
        uint32_t count = 0; // Used reference prefix; zero may be explicit admission.
    };
    // Same binding means exact same identities; pause alone cannot reselect.
    bool same_references(uint32_t slot, const TaskAuthorityRecord &record) const;
    // Stable configured region, never a TaskTable row-index lookup.
    AuthorityReference *references_at(uint32_t slot) const;
    Slot *slots_;                    // Caller arena, limits.tasks constructed slots.
    AuthorityReference *references_; // Caller arena, tasks * requirements constructed references.
    Limits limits_;                  // Immutable arena dimensions, no live Task copy.
};
} // namespace ayni::work

#endif // AYNI_CORE_WORK_TASK_TASK_AUTHORITY_H
