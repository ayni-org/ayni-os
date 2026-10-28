// ADR-0060: bounded memory facts, exact provenance and immediate deletion barriers.
#include "foundation/kernel_id.h"
#include "knowledge/memory/memory_table.h"
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <initializer_list>
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

// White-box, C-MEM-001/004 and ADR-0060: identity, trust and provenance are
// validated before a write plan exists. Rejection must preserve the plan.
TEST_F(MemoryFacts, invalid_memory_values_cannot_publish_a_write_plan) {
    struct Case {
        const char *name;
        void (*change)(MemoryValue &);
    };
    const Case cases[] = {
        {"unknown namespace", [](auto &v) { v.space = static_cast<MemoryNamespace>(0); }},
        {"zero namespace owner", [](auto &v) { v.namespace_id = 0; }},
        {"nonkernel private owner", [](auto &v) { v.namespace_id = 1; }},
        {"profile field in private memory", [](auto &v) { v.profile_field = ProfileField::name; }},
        {"unknown source", [](auto &v) { v.source = static_cast<MemorySource>(0); }},
        {"empty body", [](auto &v) { v.body = {}; }},
        {"missing body bytes", [](auto &v) { v.body = {nullptr, 1}; }},
        {"invalid source call", [](auto &v) { v.source_call_id = 1; }},
        {"unconfirmed user origin", [](auto &v) { v.confirmed = false; }},
        {"user origin claims model call",
         [](auto &v) { v.source_call_id = UINT64_C(0x100000009); }},
        {"tag count exceeds hard bound", [](auto &v) { v.labels.tag_count = memory_tag_max + 1; }},
        {"keyword count exceeds hard bound",
         [](auto &v) { v.labels.keyword_count = memory_keyword_max + 1; }},
        {"empty active tag", [](auto &v) { v.labels.tag_count = 1; }},
        {"empty active keyword", [](auto &v) { v.labels.keyword_count = 1; }},
        {"malformed dependency", [](auto &v) { v.dependencies.count = 1; }},
    };
    for (const auto &test : cases) {
        SCOPED_TRACE(test.name);
        auto candidate = value();
        test.change(candidate);
        MemoryWritePlan plan{99, 99};
        EXPECT_EQ(table.check_write(0, 0, candidate, plan), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(plan.index, 99U);
        EXPECT_EQ(plan.revision, 99U);
        EXPECT_EQ(table.size(), 0U);
    }
    // Only explicit confirmed user input may populate each supported profile field.
    for (const auto field :
         {ProfileField::name, ProfileField::language, ProfileField::interaction}) {
        auto profile = value();
        profile.space = MemoryNamespace::owner_profile;
        profile.namespace_id = 10;
        profile.profile_field = field;
        EXPECT_EQ(check_memory_value(profile, limits), AYNI_OK);
        profile.source = MemorySource::model;
        EXPECT_EQ(check_memory_value(profile, limits), AYNI_INVALID_ARGUMENT);
        profile.source = MemorySource::derived;
        EXPECT_EQ(check_memory_value(profile, limits), AYNI_INVALID_ARGUMENT);
        profile.source = MemorySource::user;
        profile.confirmed = false;
        EXPECT_EQ(check_memory_value(profile, limits), AYNI_INVALID_ARGUMENT);
    }
}

// White-box, ADR-0060: every semantic field participates in equality; a
// matching body cannot hide a changed permission, label, source or namespace.
TEST_F(MemoryFacts, memory_value_equality_includes_labels_and_exact_source_permissions) {
    auto original = value("body");
    original.source = MemorySource::derived;
    original.confirmed = false;
    ASSERT_TRUE(original.labels.tags[0].assign({"tag", 3}));
    original.labels.tag_count = 1;
    ASSERT_TRUE(original.labels.keywords[0].assign({"key", 3}));
    original.labels.keyword_count = 1;
    original.dependencies.count = 1;
    original.dependencies.items[0] = dependency(UINT64_C(0x100000009));
    ASSERT_EQ(check_memory_value(original, limits), AYNI_OK);
    EXPECT_TRUE(same_memory_value(original, original));
    const auto changes = {
        +[](MemoryValue &v) { v.space = MemoryNamespace::owner_profile; },
        +[](MemoryValue &v) { ++v.namespace_id; },
        +[](MemoryValue &v) { v.source = MemorySource::model; },
        +[](MemoryValue &v) { v.profile_field = ProfileField::name; },
        +[](MemoryValue &v) { v.confirmed = true; },
        +[](MemoryValue &v) { v.source_call_id = UINT64_C(0x100000008); },
        +[](MemoryValue &v) { v.body = {"edit", 4}; },
        +[](MemoryValue &v) { --v.body.size; },
        +[](MemoryValue &v) { v.labels.tag_count = 0; },
        +[](MemoryValue &v) { v.labels.keyword_count = 0; },
        +[](MemoryValue &v) { EXPECT_TRUE(v.labels.tags[0].assign({"new", 3})); },
        +[](MemoryValue &v) { EXPECT_TRUE(v.labels.keywords[0].assign({"new", 3})); },
        +[](MemoryValue &v) { v.dependencies.count = 0; },
        +[](MemoryValue &v) { ++v.dependencies.items[0].revision; },
        +[](MemoryValue &v) { v.dependencies.items[0].read = {UINT64_C(0x100000007), 1}; },
        +[](MemoryValue &v) { v.dependencies.items[0].egress = {UINT64_C(0x100000006), 1}; },
    };
    unsigned scenario = 0;
    for (const auto change : changes) {
        SCOPED_TRACE(scenario++);
        auto candidate = original;
        change(candidate);
        EXPECT_FALSE(same_memory_value(original, candidate));
        EXPECT_FALSE(same_memory_value(candidate, original));
    }
}

// White-box, ADR-0060: partial/invalid label arrays never replace caller
// outputs; tags and keywords each have an independent finite, unique prefix.
TEST_F(MemoryFacts, label_array_rejections_preserve_previous_outputs) {
    const ayni_string_v1 duplicate[] = {{"same", 4}, {"same", 4}};
    const ayni_string_v1 invalid[] = {{"\xc0\x80", 2}};
    MemoryLabels out{};
    out.tag_count = 1;
    ASSERT_TRUE(out.tags[0].assign({"kept", 4}));
    for (const bool tags : {false, true}) {
        for (unsigned scenario = 0; scenario < 4; ++scenario) {
            const ayni_string_v1 *bytes = nullptr;
            uint32_t count = 1;
            if (scenario == 1) {
                bytes = duplicate;
                count = 2;
            } else if (scenario == 2) {
                bytes = invalid;
            } else if (scenario == 3) {
                count = tags ? memory_tag_max + 1 : memory_keyword_max + 1;
            }
            EXPECT_FALSE(read_memory_labels(tags ? bytes : nullptr, tags ? count : 0,
                                            tags ? nullptr : bytes, tags ? 0 : count, out));
            EXPECT_EQ(out.tag_count, 1U);
            EXPECT_TRUE(out.tags[0].equals({"kept", 4}));
            EXPECT_EQ(out.keyword_count, 0U);
        }
    }
}
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

// C-MEM-005: intrinsic source and Grant identities must be complete before
// a dependency can enter a decision; context anchors never carry memory Grants.
TEST_F(MemoryFacts, malformed_dependency_identities_cannot_change_the_source_union) {
    const auto source = add(value());
    uint64_t read = 0, egress = 0;
    ASSERT_TRUE(ids.allocate(read));
    ASSERT_TRUE(ids.allocate(egress));
    auto original = dependency(source);
    original.read = {read, 1};
    original.egress = {egress, 1};
    ASSERT_TRUE(valid_dependency(original));
    const auto changes = {
        +[](MemoryDependency &v) { v.kind = static_cast<DependencyKind>(0); },
        +[](MemoryDependency &v) { v.id = 0; },
        +[](MemoryDependency &v) { v.actor_instance_id = 0; },
        +[](MemoryDependency &v) { v.revision = 0; },
        +[](MemoryDependency &v) { v.purpose = static_cast<MemoryPurpose>(0); },
        +[](MemoryDependency &v) { v.read.id = 0; },
        +[](MemoryDependency &v) { v.read.revision = 0; },
        +[](MemoryDependency &v) { v.read.id = 1; },
        +[](MemoryDependency &v) { v.egress.id = 0; },
        +[](MemoryDependency &v) { v.egress.revision = 0; },
        +[](MemoryDependency &v) { v.egress.id = 1; },
        +[](MemoryDependency &v) { v.kind = DependencyKind::context; },
        +[](MemoryDependency &v) {
            v.kind = DependencyKind::context;
            v.read = {};
        },
    };
    for (const auto change : changes) {
        auto candidate = original;
        change(candidate);
        EXPECT_FALSE(valid_dependency(candidate));
        MemoryDependencies set{};
        EXPECT_EQ(add_dependency(set, candidate, limits.dependencies), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(set.count, 0U);
        set.count = 1;
        set.items[0] = candidate;
        EXPECT_FALSE(valid_dependencies(set, limits.dependencies));
    }
    MemoryDependencies set{};
    EXPECT_EQ(add_dependency(set, original, 0), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(add_dependency(set, original, memory_dependency_max + 1), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(set.count, 0U);
    EXPECT_FALSE(valid_dependencies(set, memory_dependency_max + 1));
    auto context = original;
    context.kind = DependencyKind::context;
    ASSERT_TRUE(ids.allocate(context.id));
    context.read = {};
    context.egress = {};
    EXPECT_TRUE(valid_dependency(context));
}

// C-MEM-005: canonical reads merge only their first export permission. Two
// different exports remain distinct, while conflicting source revisions are refused.
TEST_F(MemoryFacts, dependency_set_validation_preserves_distinct_exports_and_rejects_aliases) {
    const auto source = add(value());
    uint64_t first = 0, second = 0;
    ASSERT_TRUE(ids.allocate(first));
    ASSERT_TRUE(ids.allocate(second));
    auto ref = dependency(source);
    ref.egress = {first, 1};
    MemoryDependencies set{};
    set.count = 2;
    set.items[0] = ref;
    set.items[1] = ref;
    EXPECT_FALSE(valid_dependencies(set, limits.dependencies));
    set.items[1].egress = {};
    EXPECT_FALSE(valid_dependencies(set, limits.dependencies));
    set.items[1].egress = {second, 1};
    ASSERT_TRUE(valid_dependencies(set, limits.dependencies));
    const auto accepted = set;
    auto conflict = ref;
    ++conflict.revision;
    MemoryDependencies input{};
    ASSERT_EQ(add_dependency(input, conflict, limits.dependencies), AYNI_OK);
    EXPECT_EQ(merge_dependencies(set, input, limits.dependencies), AYNI_REVISION_CONFLICT);
    EXPECT_TRUE(same_dependencies(set, accepted));
    ++set.items[1].revision;
    EXPECT_FALSE(valid_dependencies(set, limits.dependencies));
    set = accepted;
    auto malformed = input;
    malformed.items[0].actor_instance_id = 0;
    EXPECT_EQ(merge_dependencies(set, malformed, limits.dependencies), AYNI_INVALID_ARGUMENT);
    EXPECT_TRUE(same_dependencies(set, accepted));
    auto invalid_target = malformed;
    EXPECT_EQ(merge_dependencies(invalid_target, input, limits.dependencies),
              AYNI_INVALID_ARGUMENT);
    EXPECT_TRUE(same_dependencies(invalid_target, malformed));
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
