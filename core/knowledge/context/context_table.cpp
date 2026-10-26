#include "knowledge/context/context_table.h"
#include "foundation/kernel_id.h"
namespace ayni::knowledge {
bool ContextTable::find(uint64_t id, uint32_t &out) const {
    for (uint32_t index = 0; index < rows_.size(); ++index) {
        if (rows_[index].id == id) {
            out = index;
            return true;
        }
    }
    return false;
}
bool ContextTable::find_owner(uint64_t owner, uint32_t &out) const {
    for (uint32_t index = 0; index < rows_.size(); ++index) {
        if (rows_[index].owner_instance_id == owner) {
            out = index;
            return true;
        }
    }
    return false;
}
bool ContextTable::current_reference(const MemoryDependency &ref, uint32_t &out) const {
    uint32_t index = 0;
    if (ref.kind != DependencyKind::context || !find(ref.id, index) ||
        rows_[index].revision != ref.revision ||
        rows_[index].owner_instance_id != ref.actor_instance_id ||
        rows_[index].pending.action > ContextAction::publish) {
        return false;
    }
    out = index;
    return true;
}
// A pending fact has one unambiguous interpretation. Invalidation never resets the high-water
// mark or retains old sources under a new privacy revision (ADR-0060).
bool ContextTable::valid(const ContextRecord &r, uint32_t limit) {
    if (!foundation::is_kernel_id(r.id) || !foundation::is_kernel_id(r.owner_instance_id) ||
        r.revision == 0 || r.last_turn > r.generation || r.revision - 1 > r.generation ||
        r.stamp.commit_seq == 0 || r.stamp.transition_id == 0 ||
        !valid_dependencies(r.dependencies, limit) ||
        r.pending.action > ContextAction::erase_derived) {
        return false;
    }
    for (uint32_t i = 0; i < r.dependencies.count; ++i) {
        if (r.dependencies.items[i].kind == DependencyKind::context &&
            r.dependencies.items[i].id == r.id) {
            return false;
        }
    }
    const auto &p = r.pending;
    if (p.action == ContextAction::none) {
        return p.old_revision == 0 && p.turn_id == 0 && p.task_id == 0;
    }
    if (r.generation == 0) {
        return false;
    }
    if (p.action == ContextAction::publish) {
        return p.old_revision == 0 && p.turn_id != 0 && p.turn_id == r.last_turn &&
               foundation::is_kernel_id(p.task_id);
    }
    return r.revision > 1 && p.old_revision == r.revision - 1 && p.task_id == 0 &&
           r.dependencies.count == 0 &&
           (p.action == ContextAction::erase_round ? p.turn_id != 0 && p.turn_id <= r.last_turn
                                                   : p.turn_id == 0);
}
ayni_status_t ContextTable::restore(const ContextRecord &r, uint32_t limit) {
    if (!valid(r, memory_dependency_max)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (r.dependencies.count > limit) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    uint32_t index = 0;
    if (find(r.id, index)) {
        const auto &old = rows_[index];
        if (old.owner_instance_id != r.owner_instance_id || r.revision < old.revision ||
            r.generation < old.generation || r.last_turn < old.last_turn ||
            r.stamp.commit_seq <= old.stamp.commit_seq ||
            r.stamp.transition_id <= old.stamp.transition_id) {
            return AYNI_INVALID_ARGUMENT;
        }
        rows_[index] = r;
        return AYNI_OK;
    }
    if (find_owner(r.owner_instance_id, index)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (rows_.size() == rows_.capacity()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    rows_.push_back(r);
    return AYNI_OK;
}
void ContextTable::create(uint64_t id, uint64_t owner, foundation::TransitionStamp stamp) {
    ContextRecord record{};
    record.id = id;
    record.owner_instance_id = owner;
    record.stamp = stamp;
    rows_.push_back(record);
}
void ContextTable::publish(uint32_t index, uint64_t task, const MemoryDependencies &sources,
                           foundation::TransitionStamp stamp) {
    auto &record = rows_[index];
    ++record.generation;
    ++record.last_turn;
    record.dependencies = sources;
    record.pending = {ContextAction::publish, 0, record.last_turn, task};
    record.stamp = stamp;
}
void ContextTable::erase(uint32_t index, ContextAction action, uint64_t turn,
                         foundation::TransitionStamp stamp) {
    auto &record = rows_[index];
    record.pending = {action, record.revision, turn, 0};
    ++record.revision;
    ++record.generation;
    record.dependencies = {};
    record.stamp = stamp;
}
void ContextTable::acknowledge(uint32_t index, foundation::TransitionStamp stamp) {
    rows_[index].pending = {};
    rows_[index].stamp = stamp;
}
bool ContextTable::files_current() const {
    for (uint32_t index = 0; index < rows_.size(); ++index) {
        if (rows_[index].pending.action != ContextAction::none) {
            return false;
        }
    }
    return true;
}
void ContextTable::clear() {
    rows_.clear();
}
} // namespace ayni::knowledge
