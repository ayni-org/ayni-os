#include "knowledge/memory/memory_table.h"
#include "foundation/kernel_id.h"
#include "foundation/utf8.h"
#include <cstring>
#include <limits>
namespace ayni::knowledge {
namespace {
bool stamped(foundation::TransitionStamp stamp) {
    return stamp.transition_id != 0 && stamp.commit_seq != 0;
}
bool same_label(const MemoryLabel &label, ayni_string_v1 text) {
    return label.equals(text);
}
bool contains(ayni_string_v1 text, ayni_string_v1 part) {
    if (part.size == 0) {
        return true;
    }
    if (part.size > text.size || part.data == nullptr) {
        return false;
    }
    for (uint32_t at = 0; at <= text.size - part.size; ++at) {
        if (std::memcmp(text.data + at, part.data, part.size) == 0) {
            return true;
        }
    }
    return false;
}
} // namespace
std::size_t MemoryTable::storage_size(MemoryLimits limits) {
    return foundation::BoundedArray<MemoryRecord>::storage_size(limits.capacity) +
           static_cast<std::size_t>(limits.capacity) * limits.body_bytes;
}
MemoryTable::MemoryTable(void *storage, MemoryLimits limits)
    : rows_(storage, limits.capacity),
      bytes_(limits.capacity == 0
                 ? nullptr
                 : static_cast<char *>(storage) +
                       foundation::BoundedArray<MemoryRecord>::storage_size(limits.capacity)),
      limits_(limits) {}
char *MemoryTable::body_storage(uint32_t index) const {
    return bytes_ + static_cast<std::size_t>(index) * limits_.body_bytes;
}
bool MemoryTable::find(uint64_t id, uint32_t &out) const {
    if (id == 0) {
        return false;
    }
    for (uint32_t index = 0; index < size(); ++index) {
        if (at(index).id == id) {
            out = index;
            return true;
        }
    }
    return false;
}
ayni_string_v1 MemoryTable::body(uint32_t index) const {
    return {body_storage(index), at(index).body_size};
}
// Shape/namespace/CAS/capacity are decided before copying any caller bytes. Full tables
// reject new records; they never silently choose an existing memory to evict (ADR-0060).
ayni_status_t MemoryTable::check_write(uint64_t target, uint32_t expected, const MemoryValue &value,
                                       MemoryWritePlan &out) const {
    const auto form = check_memory_value(value, limits_);
    if (form != AYNI_OK) {
        return form;
    }
    uint32_t index = size();
    uint32_t revision = memory_initial_revision;
    if (target == 0) {
        if (expected != 0) {
            return AYNI_REVISION_CONFLICT;
        }
        if (rows_.full()) {
            return AYNI_CAPACITY_EXCEEDED;
        }
    } else {
        const auto status = check_live(target, expected, index);
        if (status != AYNI_OK) {
            return status;
        }
        const auto &current = at(index);
        if (current.space != value.space || current.namespace_id != value.namespace_id ||
            current.profile_field != value.profile_field) {
            return AYNI_INVALID_ARGUMENT;
        }
        revision = current.revision + 1;
    }
    out = {index, revision};
    return AYNI_OK;
}
// Consumes one unchanged admission. User input and policy proposals were copied outside
// this arena, so clearing the previous full stride cannot destroy the borrowed input.
void MemoryTable::install(const MemoryWritePlan &plan, uint64_t id, const MemoryValue &v,
                          foundation::TransitionStamp stamp) {
    MemoryRecord record{};
    record.id = id;
    record.revision = plan.revision;
    record.space = v.space;
    record.namespace_id = v.namespace_id;
    record.source = v.source;
    record.profile_field = v.profile_field;
    record.confirmed = v.confirmed;
    record.body_size = v.body.size;
    record.source_call_id = v.source_call_id;
    record.labels = v.labels;
    record.dependencies = v.dependencies;
    record.stamp = stamp;
    std::memset(body_storage(plan.index), 0, limits_.body_bytes);
    std::memcpy(body_storage(plan.index), v.body.data, v.body.size);
    if (plan.index == size()) {
        static_cast<void>(rows_.push_back(record));
    } else {
        rows_[plan.index] = record;
    }
}
ayni_status_t MemoryTable::check_live(uint64_t id, uint32_t expected, uint32_t &out) const {
    uint32_t index = 0;
    if (!find(id, index)) {
        return AYNI_NOT_FOUND;
    }
    const auto &record = at(index);
    if (record.revision != expected) {
        return AYNI_REVISION_CONFLICT;
    }
    if (record.state != MemoryState::live) {
        return AYNI_INVALID_STATE;
    }
    if (record.revision == std::numeric_limits<uint32_t>::max()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    out = index;
    return AYNI_OK;
}
void MemoryTable::confirm(uint32_t index, foundation::TransitionStamp stamp) {
    auto &record = rows_[index];
    record.confirmed = true;
    ++record.revision;
    record.stamp = stamp;
}
void MemoryTable::tombstone(uint32_t index, foundation::TransitionStamp stamp) {
    auto &record = rows_[index];
    record.state = MemoryState::tombstone;
    ++record.revision;
    record.stamp = stamp;
}
void MemoryTable::scrub(uint32_t index, foundation::TransitionStamp stamp) {
    auto &record = rows_[index];
    std::memset(body_storage(index), 0, limits_.body_bytes);
    record.body_size = 0;
    record.labels = {};
    record.dependencies = {};
    record.source_call_id = 0;
    record.confirmed = false;
    record.state = MemoryState::scrubbed;
    record.stamp = stamp;
}
void MemoryTable::remove_scrubbed(uint32_t index) {
    if (at(index).state != MemoryState::scrubbed) {
        return;
    }
    const auto last = size() - 1;
    if (index != last) {
        std::memcpy(body_storage(index), body_storage(last), limits_.body_bytes);
        rows_[index] = rows_[last];
    }
    std::memset(body_storage(last), 0, limits_.body_bytes);
    rows_[last] = {};
    rows_.remove_at(last);
}
// Iterative depth-first traversal tracks visiting and complete nodes separately, so a
// corrupt restored cycle is rejected even when the queried root only leads into it.
// Each row enters the stack once; exact revision and captured permission are rechecked.
bool MemoryTable::dependencies_current(const MemoryDependencies &deps, uint64_t durable,
                                       MemoryDependencyCheck external, uint64_t forbidden) const {
    enum class Visit : uint8_t { unseen, visiting, complete };
    struct Frame {
        const MemoryDependencies *dependencies;
        uint32_t next;
        uint32_t row;
    };
    constexpr uint32_t root_row = memory_capacity_max;
    std::array<Frame, memory_capacity_max + 1> stack{};
    std::array<Visit, memory_capacity_max> visited{};
    uint32_t depth = 0;
    stack[depth++] = {&deps, 0, root_row};
    while (depth != 0) {
        auto &frame = stack[depth - 1];
        if (frame.next == 0 && !valid_dependencies(*frame.dependencies, limits_.dependencies)) {
            return false;
        }
        if (frame.next == frame.dependencies->count) {
            if (frame.row != root_row) {
                visited[frame.row] = Visit::complete;
            }
            --depth;
            continue;
        }
        const auto &ref = frame.dependencies->items[frame.next++];
        if (external.current == nullptr || !external.current(external.context, ref)) {
            return false;
        }
        if (ref.kind == DependencyKind::context) {
            continue;
        }
        uint32_t index = 0;
        if (ref.id == forbidden || !find(ref.id, index)) {
            return false;
        }
        const auto &record = at(index);
        if (record.state != MemoryState::live || record.revision != ref.revision ||
            record.stamp.commit_seq > durable || visited[index] == Visit::visiting) {
            return false;
        }
        if (visited[index] == Visit::unseen) {
            visited[index] = Visit::visiting;
            stack[depth++] = {&record.dependencies, 0, index};
        }
    }
    return true;
}
bool MemoryTable::current(uint32_t index, uint64_t durable, MemoryDependencyCheck external) const {
    const auto &record = at(index);
    return record.state == MemoryState::live && record.stamp.commit_seq <= durable &&
           dependencies_current(record.dependencies, durable, external, record.id);
}
bool MemoryTable::matches(uint32_t index, ayni_string_v1 keyword, ayni_string_v1 tag) const {
    const auto &labels = at(index).labels;
    bool tag_match = tag.size == 0;
    for (uint32_t i = 0; i < labels.tag_count && !tag_match; ++i) {
        tag_match = same_label(labels.tags[i], tag);
    }
    if (!tag_match) {
        return false;
    }
    if (contains(body(index), keyword)) {
        return true;
    }
    for (uint32_t i = 0; i < labels.keyword_count; ++i) {
        if (same_label(labels.keywords[i], keyword)) {
            return true;
        }
    }
    return false;
}
// Restore shares typed form validation, then applies only monotone recorded versions.
// Kernel validates batch/owner/lineage references before publishing recovered state.
ayni_status_t MemoryTable::restore(const MemoryRecord &record, ayni_string_v1 text) {
    if (!foundation::is_kernel_id(record.id) || record.revision == 0 || !stamped(record.stamp) ||
        record.body_size != text.size ||
        (record.state != MemoryState::live && record.state != MemoryState::tombstone &&
         record.state != MemoryState::scrubbed)) {
        return AYNI_INVALID_ARGUMENT;
    }
    const auto v = memory_value(record, text);
    if (record.state == MemoryState::scrubbed) {
        if (!valid_memory_namespace(record.space, record.namespace_id, record.profile_field) ||
            !valid_memory_source(record.source) || text.size != 0 || record.labels.tag_count != 0 ||
            record.labels.keyword_count != 0 || record.dependencies.count != 0 ||
            record.source_call_id != 0 || record.confirmed) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else {
        const auto form = check_memory_value(v, limits_);
        if (form != AYNI_OK) {
            return form;
        }
    }
    uint32_t index = size();
    if (find(record.id, index)) {
        const auto &old = at(index);
        if (record.revision < old.revision || record.stamp.commit_seq <= old.stamp.commit_seq ||
            record.stamp.transition_id <= old.stamp.transition_id || old.space != record.space ||
            old.namespace_id != record.namespace_id || old.profile_field != record.profile_field ||
            (old.state != MemoryState::live && record.state < old.state)) {
            return AYNI_INVALID_ARGUMENT;
        }
    } else if (rows_.full()) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    std::memset(body_storage(index), 0, limits_.body_bytes);
    if (text.size != 0) {
        std::memcpy(body_storage(index), text.data, text.size);
    }
    if (index == size()) {
        static_cast<void>(rows_.push_back(record));
    } else {
        rows_[index] = record;
    }
    return AYNI_OK;
}
void MemoryTable::clear() {
    if (limits_.capacity != 0) {
        std::memset(bytes_, 0, static_cast<std::size_t>(limits_.capacity) * limits_.body_bytes);
    }
    for (uint32_t i = 0; i < size(); ++i) {
        rows_[i] = {};
    }
    rows_.clear();
}
} // namespace ayni::knowledge
