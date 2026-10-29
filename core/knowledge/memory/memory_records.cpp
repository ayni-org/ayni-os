#include "knowledge/memory/memory_records.h"
namespace ayni::knowledge {
namespace {
template <std::size_t N>
uint32_t labels_size(const std::array<MemoryLabel, N> &labels, uint32_t count) {
    uint32_t size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        size += foundation::string_field_size(labels[i].size());
    }
    return size;
}
template <std::size_t N>
void write_labels(const std::array<MemoryLabel, N> &labels, uint32_t count,
                  foundation::BodyWriter &writer) {
    writer.u32(count);
    for (uint32_t i = 0; i < count; ++i) {
        writer.bytes(labels[i].data(), labels[i].size());
    }
}
template <std::size_t N>
bool read_labels(foundation::BodyReader &reader, std::array<MemoryLabel, N> &labels,
                 uint32_t &count) {
    count = reader.u32();
    if (count > N) {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = 0;
        const auto *bytes = reader.bytes(memory_label_max_bytes, size);
        if (!labels[i].assign({reinterpret_cast<const char *>(bytes), size})) {
            return false;
        }
    }
    return true;
}
} // namespace
void write_memory_dependencies(const MemoryDependencies &deps, foundation::BodyWriter &writer) {
    writer.u32(deps.count);
    for (uint32_t i = 0; i < deps.count; ++i) {
        const auto &d = deps.items[i];
        writer.u32(static_cast<uint32_t>(d.kind));
        writer.u64(d.id);
        writer.u32(d.revision);
        writer.u64(d.actor_instance_id);
        writer.u32(static_cast<uint32_t>(d.purpose));
        writer.u64(d.read.id);
        writer.u32(d.read.revision);
        writer.u64(d.egress.id);
        writer.u32(d.egress.revision);
    }
}
bool read_memory_dependencies(foundation::BodyReader &reader, MemoryDependencies &deps) {
    deps.count = reader.u32();
    if (deps.count > memory_dependency_max) {
        return false;
    }
    for (uint32_t i = 0; i < deps.count; ++i) {
        auto &d = deps.items[i];
        d.kind = static_cast<DependencyKind>(reader.u32());
        d.id = reader.u64();
        d.revision = reader.u32();
        d.actor_instance_id = reader.u64();
        d.purpose = static_cast<MemoryPurpose>(reader.u32());
        d.read.id = reader.u64();
        d.read.revision = reader.u32();
        d.egress.id = reader.u64();
        d.egress.revision = reader.u32();
    }
    return true;
}
uint32_t memory_value_size(const MemoryValue &v) {
    return memory_value_fixed_bytes + v.body.size + labels_size(v.labels.tags, v.labels.tag_count) +
           labels_size(v.labels.keywords, v.labels.keyword_count) +
           v.dependencies.count * memory_dependency_bytes;
}
uint32_t memory_record_size(const MemoryRecord &r) {
    return memory_record_header_bytes + memory_value_size(memory_value(r, {nullptr, r.body_size}));
}
uint32_t memory_record_max(MemoryLimits limits) {
    return memory_record_header_bytes + memory_value_fixed_bytes + limits.body_bytes +
           (memory_tag_max + memory_keyword_max) *
               foundation::string_field_size(memory_label_max_bytes) +
           limits.dependencies * memory_dependency_bytes;
}
// One field order serves saved facts and exact management command identity.
void write_memory_value(const MemoryValue &v, foundation::BodyWriter &w) {
    w.u32(static_cast<uint32_t>(v.space));
    w.u64(v.namespace_id);
    w.u32(static_cast<uint32_t>(v.source));
    w.u32(static_cast<uint32_t>(v.profile_field));
    w.u32(v.confirmed ? 1 : 0);
    w.u64(v.source_call_id);
    w.bytes(v.body.data, v.body.size);
    write_labels(v.labels.tags, v.labels.tag_count, w);
    write_labels(v.labels.keywords, v.labels.keyword_count, w);
    write_memory_dependencies(v.dependencies, w);
}
// Count limits precede all array access. Caller checks final framing and shared value rules.
bool read_memory_value(foundation::BodyReader &r, MemoryValue &out) {
    MemoryValue v{};
    v.space = static_cast<MemoryNamespace>(r.u32());
    v.namespace_id = r.u64();
    v.source = static_cast<MemorySource>(r.u32());
    v.profile_field = static_cast<ProfileField>(r.u32());
    const auto confirmed = r.u32();
    if (confirmed > 1) {
        return false;
    }
    v.confirmed = confirmed != 0;
    v.source_call_id = r.u64();
    v.body.data = reinterpret_cast<const char *>(r.bytes(memory_body_max, v.body.size));
    if (!read_labels(r, v.labels.tags, v.labels.tag_count) ||
        !read_labels(r, v.labels.keywords, v.labels.keyword_count) ||
        !read_memory_dependencies(r, v.dependencies)) {
        return false;
    }
    out = v;
    return true;
}
void write_memory_record(const MemoryRecord &r, ayni_string_v1 text, uint8_t *out) {
    foundation::BodyWriter w(out, memory_record_size(r));
    w.u64(r.id);
    w.u32(r.revision);
    w.u32(static_cast<uint32_t>(r.state));
    w.u64(r.stamp.transition_id);
    w.u64(r.stamp.commit_seq);
    write_memory_value(memory_value(r, text), w);
}
// Publish a decoded row only after exact framing; table restore owns semantic validation.
bool read_memory_record(const uint8_t *body, uint32_t size, DecodedMemory &out) {
    if (body == nullptr || size < memory_record_header_bytes + memory_value_fixed_bytes) {
        return false;
    }
    foundation::BodyReader reader(body, size);
    DecodedMemory decoded{};
    auto &record = decoded.record;
    record.id = reader.u64();
    record.revision = reader.u32();
    record.state = static_cast<MemoryState>(reader.u32());
    record.stamp.transition_id = reader.u64();
    record.stamp.commit_seq = reader.u64();
    MemoryValue value{};
    if (!read_memory_value(reader, value) || !reader.ok()) {
        return false;
    }
    record.space = value.space;
    record.namespace_id = value.namespace_id;
    record.source = value.source;
    record.profile_field = value.profile_field;
    record.confirmed = value.confirmed;
    record.source_call_id = value.source_call_id;
    record.body_size = value.body.size;
    record.labels = value.labels;
    record.dependencies = value.dependencies;
    decoded.body = value.body;
    out = decoded;
    return true;
}
} // namespace ayni::knowledge
