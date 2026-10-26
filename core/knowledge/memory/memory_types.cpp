#include "knowledge/memory/memory_types.h"
#include "foundation/kernel_id.h"
#include "foundation/utf8.h"
#include <cstring>
namespace ayni::knowledge {
namespace {
bool same_grant(MemoryGrantRef a, MemoryGrantRef b) {
    return a.id == b.id && a.revision == b.revision;
}
bool valid_grant(MemoryGrantRef ref) {
    return ref.id == 0 ? ref.revision == 0 : foundation::is_kernel_id(ref.id) && ref.revision != 0;
}
template <std::size_t Size>
bool unique_labels(const std::array<MemoryLabel, Size> &labels, uint32_t count) {
    if (count > Size) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (labels[i].size() == 0) {
            return false;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (labels[i].equals({labels[j].data(), labels[j].size()})) {
                return false;
            }
        }
    }
    return true;
}
template <std::size_t N>
bool copy_labels(const ayni_string_v1 *input, uint32_t count, std::array<MemoryLabel, N> &out) {
    if (count > N || (count != 0 && input == nullptr)) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!out[i].assign(input[i])) {
            return false;
        }
    }
    return true;
}
} // namespace
bool valid_memory_namespace(MemoryNamespace space, uint64_t id, ProfileField field) {
    if (space == MemoryNamespace::private_agent) {
        return foundation::is_kernel_id(id) && field == ProfileField::none;
    }
    return space == MemoryNamespace::owner_profile && id != 0 &&
           (field == ProfileField::name || field == ProfileField::language ||
            field == ProfileField::interaction);
}
bool valid_memory_source(MemorySource source) {
    return source == MemorySource::user || source == MemorySource::model ||
           source == MemorySource::derived;
}
ayni_status_t check_memory_value(const MemoryValue &v, MemoryLimits limits) {
    if (v.body.size > limits.body_bytes) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    if (!valid_memory_namespace(v.space, v.namespace_id, v.profile_field) ||
        !valid_memory_source(v.source) || v.body.size == 0 ||
        !foundation::valid_utf8(v.body.data, v.body.size) ||
        (v.source_call_id != 0 && !foundation::is_kernel_id(v.source_call_id)) ||
        !valid_memory_labels(v.labels) ||
        !valid_dependencies(v.dependencies, limits.dependencies)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (v.space == MemoryNamespace::owner_profile &&
        (v.source != MemorySource::user || !v.confirmed || v.dependencies.count != 0)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (v.source == MemorySource::user &&
        (!v.confirmed || v.dependencies.count != 0 || v.source_call_id != 0)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return AYNI_OK;
}
MemoryValue memory_value(const MemoryRecord &r, ayni_string_v1 body) {
    return {r.space,          r.namespace_id, r.source, r.profile_field, r.confirmed,
            r.source_call_id, body,           r.labels, r.dependencies};
}
bool same_memory_value(const MemoryValue &a, const MemoryValue &b) {
    if (a.space != b.space || a.namespace_id != b.namespace_id || a.source != b.source ||
        a.profile_field != b.profile_field || a.confirmed != b.confirmed ||
        a.source_call_id != b.source_call_id || a.body.size != b.body.size ||
        a.labels.tag_count != b.labels.tag_count ||
        a.labels.keyword_count != b.labels.keyword_count ||
        a.dependencies.count != b.dependencies.count ||
        (a.body.size != 0 && std::memcmp(a.body.data, b.body.data, a.body.size) != 0)) {
        return false;
    }
    for (uint32_t i = 0; i < a.labels.tag_count; ++i) {
        if (!a.labels.tags[i].equals({b.labels.tags[i].data(), b.labels.tags[i].size()})) {
            return false;
        }
    }
    for (uint32_t i = 0; i < a.labels.keyword_count; ++i) {
        if (!a.labels.keywords[i].equals(
                {b.labels.keywords[i].data(), b.labels.keywords[i].size()})) {
            return false;
        }
    }
    for (uint32_t i = 0; i < a.dependencies.count; ++i) {
        if (!same_dependency(a.dependencies.items[i], b.dependencies.items[i])) {
            return false;
        }
    }
    return true;
}
bool MemoryLabel::assign(ayni_string_v1 text) {
    if (text.size == 0 || text.size > bytes_.size() ||
        !foundation::valid_utf8(text.data, text.size)) {
        return false;
    }
    std::memmove(bytes_.data(), text.data, text.size);
    std::memset(bytes_.data() + text.size, 0, bytes_.size() - text.size);
    size_ = text.size;
    return true;
}
bool MemoryLabel::equals(ayni_string_v1 text) const {
    return text.size == size_ &&
           (size_ == 0 ||
            (text.data != nullptr && std::memcmp(bytes_.data(), text.data, size_) == 0));
}
bool same_memory_read(const MemoryDependency &a, const MemoryDependency &b) {
    return a.kind == b.kind && a.id == b.id && a.revision == b.revision &&
           a.actor_instance_id == b.actor_instance_id && a.purpose == b.purpose &&
           same_grant(a.read, b.read);
}
bool same_dependency(const MemoryDependency &a, const MemoryDependency &b) {
    return same_memory_read(a, b) && same_grant(a.egress, b.egress);
}
namespace {
// One canonical read may acquire its first export Grant; two distinct exports remain separate.
bool mergeable_dependency(const MemoryDependency &a, const MemoryDependency &b) {
    return same_memory_read(a, b) &&
           (a.egress.id == 0 || b.egress.id == 0 || same_grant(a.egress, b.egress));
}
} // namespace
bool valid_dependency(const MemoryDependency &d) {
    if ((d.kind != DependencyKind::memory && d.kind != DependencyKind::context) ||
        !foundation::is_kernel_id(d.id) || !foundation::is_kernel_id(d.actor_instance_id) ||
        d.revision == 0 ||
        (d.purpose != MemoryPurpose::context && d.purpose != MemoryPurpose::organize) ||
        !valid_grant(d.read) || !valid_grant(d.egress)) {
        return false;
    }
    return d.kind != DependencyKind::context || (d.read.id == 0 && d.egress.id == 0);
}
ayni_status_t add_dependency(MemoryDependencies &set, const MemoryDependency &d, uint32_t limit) {
    if (limit == 0 || limit > memory_dependency_max || set.count > limit || !valid_dependency(d)) {
        return AYNI_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < set.count; ++i) {
        if (mergeable_dependency(set.items[i], d)) {
            // Adding the first export permission refines this read, never drops a captured Grant.
            if (set.items[i].egress.id == 0) {
                set.items[i].egress = d.egress;
            }
            return AYNI_OK;
        }
        if (set.items[i].kind == d.kind && set.items[i].id == d.id &&
            set.items[i].revision != d.revision) {
            return AYNI_REVISION_CONFLICT;
        }
    }
    if (set.count == limit) {
        return AYNI_CAPACITY_EXCEEDED;
    }
    set.items[set.count++] = d;
    return AYNI_OK;
}
bool contains_dependencies(const MemoryDependencies &set, const MemoryDependencies &subset) {
    for (uint32_t i = 0; i < subset.count; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < set.count; ++j) {
            const auto &previous = subset.items[i];
            const auto &current = set.items[j];
            found =
                found || (same_memory_read(previous, current) &&
                          (previous.egress.id == 0 || same_grant(previous.egress, current.egress)));
        }
        if (!found) {
            return false;
        }
    }
    return true;
}
ayni_status_t merge_dependencies(MemoryDependencies &set, const MemoryDependencies &input,
                                 uint32_t limit) {
    if (!valid_dependencies(input, limit)) {
        return AYNI_INVALID_ARGUMENT;
    }
    if (!valid_dependencies(set, limit)) {
        return AYNI_INVALID_ARGUMENT;
    }
    auto merged = set;
    for (uint32_t i = 0; i < input.count; ++i) {
        const auto status = add_dependency(merged, input.items[i], limit);
        if (status != AYNI_OK) {
            return status;
        }
    }
    set = merged;
    return AYNI_OK;
}
bool valid_dependencies(const MemoryDependencies &set, uint32_t limit) {
    if (limit > memory_dependency_max || set.count > limit) {
        return false;
    }
    for (uint32_t i = 0; i < set.count; ++i) {
        if (!valid_dependency(set.items[i])) {
            return false;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (mergeable_dependency(set.items[i], set.items[j]) ||
                (set.items[i].kind == set.items[j].kind && set.items[i].id == set.items[j].id &&
                 set.items[i].revision != set.items[j].revision)) {
                return false;
            }
        }
    }
    return true;
}
bool read_memory_labels(const ayni_string_v1 *tags, uint32_t tag_count,
                        const ayni_string_v1 *keywords, uint32_t keyword_count, MemoryLabels &out) {
    MemoryLabels labels{};
    labels.tag_count = tag_count;
    labels.keyword_count = keyword_count;
    if (!copy_labels(tags, tag_count, labels.tags) ||
        !copy_labels(keywords, keyword_count, labels.keywords) || !valid_memory_labels(labels)) {
        return false;
    }
    out = labels;
    return true;
}
bool valid_memory_labels(const MemoryLabels &labels) {
    return unique_labels(labels.tags, labels.tag_count) &&
           unique_labels(labels.keywords, labels.keyword_count);
}
} // namespace ayni::knowledge
