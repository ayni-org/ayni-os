// ADR-0060: bounded memory facts, exact provenance and immediate deletion barriers.
#include "foundation/kernel_id.h"
#include "knowledge/memory/memory_table.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>
namespace {
using namespace ayni::knowledge;
using ayni::foundation::TransitionStamp;
class MemoryFacts : public testing::Test {
  protected:
    static constexpr MemoryLimits limits{4, 64, 3};
    std::vector<std::max_align_t> arena = std::vector<std::max_align_t>(
        (MemoryTable::storage_size(limits) + sizeof(std::max_align_t) - 1) /
        sizeof(std::max_align_t));
    MemoryTable table{arena.data(), limits};
    ayni::foundation::IdAllocator ids;
    uint64_t agent = 0;
    uint64_t seq = 0;
    MemoryDependencyCheck available{nullptr,
                                    [](const void *, const MemoryDependency &) { return true; }};
    void SetUp() override {
        ids.start(1);
        ASSERT_TRUE(ids.allocate(agent));
    }
    TransitionStamp stamp() {
        ++seq;
        return {seq, seq};
    }
    MemoryValue value(const char *text = "记住这条偏好") {
        return {MemoryNamespace::private_agent,
                agent,
                MemorySource::user,
                ProfileField::none,
                true,
                0,
                {text, static_cast<uint32_t>(std::strlen(text))},
                {},
                {}};
    }
    uint64_t add(MemoryValue v) {
        MemoryWritePlan plan{};
        EXPECT_EQ(table.check_write(0, 0, v, plan), AYNI_OK);
        uint64_t id = 0;
        EXPECT_TRUE(ids.allocate(id));
        table.install(plan, id, v, stamp());
        return id;
    }
    MemoryDependency dependency(uint64_t id, uint32_t revision = 1) {
        return {DependencyKind::memory, id, revision, agent, MemoryPurpose::context, {}, {}};
    }
    uint32_t index(uint64_t id) {
        uint32_t at = 0;
        EXPECT_TRUE(table.find(id, at));
        return at;
    }
};
TEST_F(MemoryFacts, copies_bounded_text_and_rejects_full_table_without_eviction) {
    char text[] = "original";
    const auto id = add(value(text));
    text[0] = 'X';
    EXPECT_EQ(std::string(table.body(index(id)).data, table.body(index(id)).size), "original");
    for (uint32_t i = 1; i < limits.capacity; ++i) {
        add(value());
    }
    MemoryWritePlan untouched{99, 99};
    EXPECT_EQ(table.check_write(0, 0, value(), untouched), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(untouched.index, 99U);
    EXPECT_EQ(table.at(0).id, id);
}
TEST_F(MemoryFacts, edits_require_exact_revision_and_keep_namespace_identity) {
    const auto id = add(value());
    auto replacement = value("short");
    MemoryWritePlan plan{};
    EXPECT_EQ(table.check_write(id, 2, replacement, plan), AYNI_REVISION_CONFLICT);
    replacement.namespace_id = id;
    EXPECT_EQ(table.check_write(id, 1, replacement, plan), AYNI_INVALID_ARGUMENT);
    replacement.namespace_id = agent;
    ASSERT_EQ(table.check_write(id, 1, replacement, plan), AYNI_OK);
    table.install(plan, id, replacement, stamp());
    EXPECT_EQ(table.at(0).revision, 2U);
    EXPECT_EQ(table.body(0).size, 5U);
}
TEST_F(MemoryFacts, byte_limit_precedes_utf8_scan_and_valid_text_uses_utf8_bytes) {
    auto v = value("你");
    MemoryWritePlan plan{};
    EXPECT_EQ(table.check_write(0, 0, v, plan), AYNI_OK);
    v.body = {"\xc0\x80", 2};
    EXPECT_EQ(table.check_write(0, 0, v, plan), AYNI_INVALID_ARGUMENT);
    v.body = {nullptr, limits.body_bytes + 1};
    EXPECT_EQ(table.check_write(0, 0, v, plan), AYNI_CAPACITY_EXCEEDED);
}
TEST_F(MemoryFacts, confirmation_preserves_model_origin_and_source_dependencies) {
    const auto source = add(value());
    auto inferred = value("inferred");
    inferred.source = MemorySource::model;
    inferred.confirmed = false;
    ASSERT_EQ(add_dependency(inferred.dependencies, dependency(source), limits.dependencies),
              AYNI_OK);
    const auto id = add(inferred);
    table.confirm(index(id), stamp());
    const auto &record = table.at(index(id));
    EXPECT_TRUE(record.confirmed);
    EXPECT_EQ(record.source, MemorySource::model);
    EXPECT_EQ(record.dependencies.count, 1U);
    EXPECT_EQ(record.dependencies.items[0].id, source);
    EXPECT_EQ(record.revision, 2U);
}
TEST_F(MemoryFacts, deleting_source_immediately_invalidates_transitive_derivations) {
    const auto original = add(value("source"));
    auto first = value("first summary");
    first.source = MemorySource::derived;
    first.confirmed = false;
    ASSERT_EQ(add_dependency(first.dependencies, dependency(original), limits.dependencies),
              AYNI_OK);
    const auto summary = add(first);
    auto second = value("second summary");
    second.source = MemorySource::derived;
    second.confirmed = false;
    ASSERT_EQ(add_dependency(second.dependencies, dependency(summary), limits.dependencies),
              AYNI_OK);
    const auto derived = add(second);
    EXPECT_TRUE(table.current(index(derived), seq, available));
    table.tombstone(index(original), stamp());
    EXPECT_FALSE(table.current(index(original), seq, available));
    EXPECT_FALSE(table.current(index(derived), seq, available));
    EXPECT_GT(table.body(index(original)).size, 0U); // Barrier precedes bounded body cleanup.
    table.scrub(index(original), stamp());
    EXPECT_EQ(table.body(index(original)).size, 0U);
    table.remove_scrubbed(index(original));
    EXPECT_FALSE(table.current(index(derived), seq, available));
}
TEST_F(MemoryFacts, changed_source_and_captured_permission_never_select_a_replacement) {
    const auto original = add(value());
    auto derived = value("derived");
    derived.source = MemorySource::derived;
    derived.confirmed = false;
    ASSERT_EQ(add_dependency(derived.dependencies, dependency(original), limits.dependencies),
              AYNI_OK);
    const auto id = add(derived);
    MemoryDependencyCheck denied{nullptr,
                                 [](const void *, const MemoryDependency &) { return false; }};
    EXPECT_FALSE(table.current(index(id), seq, denied));
    table.confirm(index(original), stamp());
    EXPECT_FALSE(table.current(index(id), seq, available));
}
TEST_F(MemoryFacts, dependency_union_is_atomic_and_never_truncates) {
    MemoryDependencies set{};
    const auto first = add(value());
    const auto second = add(value());
    ASSERT_EQ(add_dependency(set, dependency(first), 1), AYNI_OK);
    EXPECT_EQ(add_dependency(set, dependency(first), 1), AYNI_OK);
    EXPECT_EQ(add_dependency(set, dependency(first, 2), 1), AYNI_REVISION_CONFLICT);
    MemoryDependencies input{};
    ASSERT_EQ(add_dependency(input, dependency(second), 1), AYNI_OK);
    EXPECT_EQ(merge_dependencies(set, input, 1), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(set.count, 1U);
    EXPECT_EQ(set.items[0].id, first);
}
TEST_F(MemoryFacts, exact_tags_and_keywords_combine_with_body_search) {
    auto v = value("likes green tea");
    ASSERT_TRUE(v.labels.tags[0].assign({"food", 4}));
    v.labels.tag_count = 1;
    ASSERT_TRUE(v.labels.keywords[0].assign({"drink", 5}));
    v.labels.keyword_count = 1;
    add(v);
    EXPECT_TRUE(table.matches(0, {"green", 5}, {"food", 4}));
    EXPECT_TRUE(table.matches(0, {"drink", 5}, {}));
    EXPECT_FALSE(table.matches(0, {"Green", 5}, {}));
    EXPECT_FALSE(table.matches(0, {}, {"foo", 3}));
}
TEST_F(MemoryFacts, scrub_and_remove_clear_search_content_from_retired_slots) {
    auto v = value("unique-body-marker");
    ASSERT_TRUE(v.labels.tags[0].assign({"unique-tag-marker", 17}));
    v.labels.tag_count = 1;
    const auto id = add(v);
    add(value("survivor"));
    table.tombstone(index(id), stamp());
    table.scrub(index(id), stamp());
    table.remove_scrubbed(index(id));
    // White-box: the entire owned arena must not retain retired body or search labels.
    const std::string bytes(reinterpret_cast<const char *>(arena.data()),
                            MemoryTable::storage_size(limits));
    EXPECT_EQ(bytes.find("unique-body-marker"), std::string::npos);
    EXPECT_EQ(bytes.find("unique-tag-marker"), std::string::npos);
    table.clear();
    const std::string cleared(reinterpret_cast<const char *>(arena.data()),
                              MemoryTable::storage_size(limits));
    EXPECT_EQ(cleared.find("survivor"), std::string::npos);
}
TEST_F(MemoryFacts, prospective_edit_cannot_create_a_dependency_cycle) {
    const auto source = add(value());
    auto summary = value("summary");
    summary.source = MemorySource::derived;
    summary.confirmed = false;
    ASSERT_EQ(add_dependency(summary.dependencies, dependency(source), limits.dependencies),
              AYNI_OK);
    const auto derived = add(summary);
    MemoryDependencies edit{};
    ASSERT_EQ(add_dependency(edit, dependency(derived), limits.dependencies), AYNI_OK);
    EXPECT_FALSE(table.dependencies_current(edit, seq, available, source));
}

// ADR-0060: first export refines a captured read; replacing nonempty authority is forbidden.
TEST_F(MemoryFacts, first_export_permission_extends_lineage_without_duplicate_read_slots) {
    const auto id = add(value());
    auto read = dependency(id);
    MemoryDependencies original{};
    ASSERT_EQ(add_dependency(original, read, limits.dependencies), AYNI_OK);
    uint64_t grant = 0;
    ASSERT_TRUE(ids.allocate(grant));
    auto exported = read;
    exported.egress = {grant, 1};
    auto updated = original;
    ASSERT_EQ(add_dependency(updated, exported, limits.dependencies), AYNI_OK);
    EXPECT_EQ(updated.count, 1U);
    EXPECT_TRUE(contains_dependencies(updated, original));
    EXPECT_FALSE(contains_dependencies(original, updated));
    EXPECT_TRUE(valid_dependencies(updated, limits.dependencies));
    ASSERT_EQ(add_dependency(updated, read, limits.dependencies), AYNI_OK);
    EXPECT_EQ(updated.count, 1U);
    EXPECT_EQ(updated.items[0].egress.id, grant);
}
// C-STORE-011: valid source bytes are distinct from this boot's lower dependency budget.
TEST_F(MemoryFacts, lowering_dependency_capacity_does_not_classify_valid_history_as_corrupt) {
    auto derived = value();
    derived.source = MemorySource::derived;
    derived.confirmed = false;
    derived.dependencies.items[derived.dependencies.count++] = dependency(add(value()));
    derived.dependencies.items[derived.dependencies.count++] = dependency(add(value()));
    const auto id = add(derived);
    MemoryLimits smaller = limits;
    smaller.dependencies = 1;
    std::vector<std::max_align_t> storage(arena.size());
    MemoryTable restored(storage.data(), smaller);
    EXPECT_EQ(restored.restore(table.at(index(id)), table.body(index(id))), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(restored.size(), 0U);
}
} // namespace
