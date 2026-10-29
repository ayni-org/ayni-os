#include "work/provenance/provenance_table.h"
#include "foundation/kernel_id.h"
#include <new>
namespace ayni::work {
std::size_t ProvenanceTable::storage_size(Limits limits) {
    return static_cast<std::size_t>(limits.goals + limits.tasks) * sizeof(ProvenanceRecord);
}
ProvenanceTable::ProvenanceTable(void *storage, Limits limits)
    : records_(static_cast<ProvenanceRecord *>(storage)), limits_(limits) {
    for (uint32_t i = 0; i < capacity(); ++i) {
        new (records_ + i) ProvenanceRecord{};
    }
}
bool ProvenanceTable::valid_slot(ProvenanceKind kind, uint32_t slot) const {
    return kind == ProvenanceKind::goal ? slot < limits_.goals
                                        : kind == ProvenanceKind::task && slot < limits_.tasks;
}
uint32_t ProvenanceTable::index(ProvenanceKind kind, uint32_t slot) const {
    return kind == ProvenanceKind::goal ? slot : limits_.goals + slot;
}
const ProvenanceRecord *ProvenanceTable::find(ProvenanceKind kind, uint32_t slot,
                                              uint64_t id) const {
    if (!valid_slot(kind, slot) || id == 0) {
        return nullptr;
    }
    const auto &record = records_[index(kind, slot)];
    return record.id == id ? &record : nullptr;
}
bool ProvenanceTable::expect(ProvenanceKind kind, uint32_t slot, uint64_t id) {
    if (!valid_slot(kind, slot)) {
        return false;
    }
    const auto &old = records_[index(kind, slot)];
    if (old.id != 0) {
        return old.id == id && old.content == ContentState::awaiting_record;
    }
    // Task and checkpoint REQUIRED markers may both reserve this same pending identity.
    records_[index(kind, slot)] = {id, kind, ContentState::awaiting_record, {}, {}};
    return true;
}
bool ProvenanceTable::complete() const {
    for (uint32_t i = 0; i < capacity(); ++i) {
        if (records_[i].content == ContentState::awaiting_record) {
            return false;
        }
    }
    return true;
}
void ProvenanceTable::assign(ProvenanceKind kind, uint32_t slot, uint64_t id,
                             const knowledge::MemoryDependencies &dependencies,
                             foundation::TransitionStamp stamp, ContextOrigin origin) {
    records_[index(kind, slot)] = {id, kind, ContentState::available, dependencies, stamp, origin};
}
void ProvenanceTable::erase(ProvenanceKind kind, uint32_t slot, uint64_t id,
                            foundation::TransitionStamp stamp) {
    records_[index(kind, slot)] = {id, kind, ContentState::erased, {}, stamp};
}
void ProvenanceTable::clear(ProvenanceKind kind, uint32_t slot) {
    if (valid_slot(kind, slot)) {
        records_[index(kind, slot)] = {};
    }
}
void ProvenanceTable::clear() {
    for (uint32_t i = 0; i < capacity(); ++i) {
        records_[i] = {};
    }
}
// Restore uses owner slots, exact IDs and monotone content state; Kernel proves work/stamp pairing.
ayni_status_t ProvenanceTable::restore(uint32_t slot, const ProvenanceRecord &record,
                                       uint32_t dependency_limit) {
    if (record.origin.id != 0) {
        bool present = false;
        for (uint32_t i = 0; i < record.dependencies.count && i < knowledge::memory_dependency_max;
             ++i) {
            const auto &ref = record.dependencies.items[i];
            present = present || (ref.kind == knowledge::DependencyKind::context &&
                                  ref.id == record.origin.id);
        }
        if (record.kind != ProvenanceKind::goal || record.content != ContentState::available ||
            !present) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else if (!record.origin.valid()) {
        return AYNI_INVALID_ARGUMENT;
    }
    if ((record.kind != ProvenanceKind::goal && record.kind != ProvenanceKind::task) ||
        !foundation::is_kernel_id(record.id) || record.stamp.transition_id == 0 ||
        record.stamp.commit_seq == 0 ||
        (record.content != ContentState::available && record.content != ContentState::erased) ||
        !knowledge::valid_dependencies(record.dependencies, knowledge::memory_dependency_max)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (record.dependencies.count > dependency_limit || !valid_slot(record.kind, slot)) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    const auto &old = records_[index(record.kind, slot)];
    if (old.content == ContentState::awaiting_record && old.id != record.id) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (old.id != 0 && old.content != ContentState::awaiting_record &&
        (old.id != record.id || old.stamp.transition_id >= record.stamp.transition_id ||
         old.stamp.commit_seq >= record.stamp.commit_seq || old.content > record.content ||
         !knowledge::contains_dependencies(record.dependencies, old.dependencies))) {
        return AYNI_INVALID_ARGUMENT;
    }
    records_[index(record.kind, slot)] = record;
    return AYNI_OK;
}
} // namespace ayni::work
