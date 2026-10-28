// White-box source metadata owner boundaries, C-MEM-005/008 and C-STORE-007/009.
// Work supplies slots; this owner maintains exact identities, unions and erasure state.
#include "foundation/kernel_id.h"
#include "work/provenance/provenance_records.h"

#include <algorithm>
#include <gtest/gtest.h>
#include <vector>

namespace {
using namespace ayni::work;
using namespace ayni::knowledge;

class ProvenanceFacts : public ::testing::Test {
  protected:
    void SetUp() override {
        ids.start(1);
        for (auto *id : {&goal, &task, &actor, &memory, &context}) {
            ASSERT_TRUE(ids.allocate(*id));
        }
    }
    ProvenanceRecord fact() const {
        ProvenanceRecord record{goal, ProvenanceKind::goal, ContentState::available, {}, {1, 1}};
        record.dependencies.count = 1;
        record.dependencies.items[0] = {DependencyKind::memory, memory, 1, actor,
                                        MemoryPurpose::context, {},     {}};
        return record;
    }
    static constexpr ProvenanceTable::Limits limits{1, 1};
    static constexpr uint32_t dependency_limit = 3;
    std::vector<std::max_align_t> arena = std::vector<std::max_align_t>(
        (ProvenanceTable::storage_size(limits) + sizeof(std::max_align_t) - 1) /
        sizeof(std::max_align_t));
    ProvenanceTable table{arena.data(), limits};
    ayni::foundation::IdAllocator ids;
    uint64_t goal = 0, task = 0, actor = 0, memory = 0, context = 0;
};

TEST_F(ProvenanceFacts, malformed_rows_do_not_claim_a_work_slot) {
    const auto original = fact();
    const auto changes = {
        +[](ProvenanceRecord &v) { v.id = 0; },
        +[](ProvenanceRecord &v) { v.kind = static_cast<ProvenanceKind>(0); },
        +[](ProvenanceRecord &v) { v.content = ContentState::awaiting_record; },
        +[](ProvenanceRecord &v) { v.stamp.transition_id = 0; },
        +[](ProvenanceRecord &v) { v.stamp.commit_seq = 0; },
        +[](ProvenanceRecord &v) { v.dependencies.count = memory_dependency_max + 1; },
        +[](ProvenanceRecord &v) { v.dependencies.items[0].actor_instance_id = 0; },
        +[](ProvenanceRecord &v) { v.origin.generation = 1; },
        +[](ProvenanceRecord &v) { v.origin.id = v.id; },
    };
    for (const auto change : changes) {
        auto candidate = original;
        change(candidate);
        EXPECT_EQ(table.restore(0, candidate, dependency_limit), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(table.find(ProvenanceKind::goal, 0, goal), nullptr);
        EXPECT_TRUE(table.complete());
    }
    ASSERT_EQ(table.restore(0, original, dependency_limit), AYNI_OK);
    const auto *saved = table.find(ProvenanceKind::goal, 0, goal);
    ASSERT_NE(saved, nullptr);
    EXPECT_TRUE(same_dependencies(saved->dependencies, original.dependencies));
}

TEST_F(ProvenanceFacts, history_cannot_retarget_regress_or_drop_captured_sources) {
    const auto original = fact();
    ASSERT_EQ(table.restore(0, original, dependency_limit), AYNI_OK);
    auto next = original;
    next.stamp = {2, 2};
    for (unsigned fault = 0; fault < 4; ++fault) {
        SCOPED_TRACE(fault);
        auto candidate = next;
        if (fault == 0) {
            candidate.id = task;
        } else if (fault == 1) {
            candidate.stamp.transition_id = original.stamp.transition_id;
        } else if (fault == 2) {
            candidate.stamp.commit_seq = original.stamp.commit_seq;
        } else {
            candidate.dependencies = {};
        }
        EXPECT_EQ(table.restore(0, candidate, dependency_limit), AYNI_INVALID_ARGUMENT);
        const auto *saved = table.find(ProvenanceKind::goal, 0, goal);
        ASSERT_NE(saved, nullptr);
        EXPECT_EQ(saved->stamp.commit_seq, original.stamp.commit_seq);
        EXPECT_TRUE(same_dependencies(saved->dependencies, original.dependencies));
    }
    ASSERT_EQ(table.restore(0, next, dependency_limit), AYNI_OK);
    // Erasure has its own owner operation, rather than an available-row journal replay.
    table.erase(ProvenanceKind::goal, 0, goal, {3, 3});
    const auto *erased = table.find(ProvenanceKind::goal, 0, goal);
    ASSERT_NE(erased, nullptr);
    EXPECT_EQ(erased->content, ContentState::erased);
    EXPECT_EQ(erased->dependencies.count, 0U);
    next.stamp = {4, 4};
    EXPECT_EQ(table.restore(0, next, dependency_limit), AYNI_INVALID_ARGUMENT);
    erased = table.find(ProvenanceKind::goal, 0, goal);
    ASSERT_NE(erased, nullptr);
    EXPECT_EQ(erased->content, ContentState::erased);
    table.clear(ProvenanceKind::goal, 0);
    EXPECT_EQ(table.find(ProvenanceKind::goal, 0, goal), nullptr);
    EXPECT_TRUE(table.complete());
}

TEST_F(ProvenanceFacts, required_markers_close_only_with_the_same_identity_and_capacity) {
    ASSERT_TRUE(table.expect(ProvenanceKind::goal, 0, goal));
    EXPECT_TRUE(table.expect(ProvenanceKind::goal, 0, goal));
    EXPECT_FALSE(table.expect(ProvenanceKind::goal, 0, task));
    EXPECT_FALSE(table.complete());
    auto other = fact();
    other.id = task;
    EXPECT_EQ(table.restore(0, other, dependency_limit), AYNI_INVALID_ARGUMENT);
    EXPECT_FALSE(table.complete());
    ASSERT_EQ(table.restore(0, fact(), dependency_limit), AYNI_OK);
    EXPECT_TRUE(table.complete());
    EXPECT_FALSE(table.expect(ProvenanceKind::goal, 0, goal));
    ProvenanceTable disabled(nullptr, {0, 0});
    EXPECT_FALSE(disabled.expect(ProvenanceKind::goal, 0, goal));
    EXPECT_EQ(disabled.restore(0, fact(), 0), AYNI_CAPACITY_EXCEEDED);
    EXPECT_EQ(disabled.restore(0, fact(), dependency_limit), AYNI_CAPACITY_EXCEEDED);
    EXPECT_TRUE(disabled.complete());
}

TEST_F(ProvenanceFacts, context_origins_require_a_matching_goal_source_and_exact_codec_version) {
    auto original = fact();
    original.origin = {context, 2};
    original.dependencies.items[0].kind = DependencyKind::context;
    original.dependencies.items[0].id = context;
    ASSERT_TRUE(valid_dependencies(original.dependencies, dependency_limit));
    ASSERT_EQ(table.restore(0, original, dependency_limit), AYNI_OK);
    const auto changes = {
        +[](ProvenanceRecord &v) { v.kind = ProvenanceKind::task; },
        +[](ProvenanceRecord &v) { v.content = ContentState::erased; },
        +[](ProvenanceRecord &v) { v.dependencies = {}; },
        +[](ProvenanceRecord &v) { v.origin.id = 0; },
    };
    for (const auto change : changes) {
        auto candidate = original;
        candidate.stamp = {2, 2};
        change(candidate);
        EXPECT_EQ(table.restore(0, candidate, dependency_limit), AYNI_INVALID_ARGUMENT);
        const auto *saved = table.find(ProvenanceKind::goal, 0, goal);
        ASSERT_NE(saved, nullptr);
        EXPECT_EQ(saved->origin.id, context);
    }
    std::vector<uint8_t> bytes(provenance_record_size(original));
    write_provenance_record(original, bytes.data());
    const auto version = provenance_record_version_for(original);
    ASSERT_EQ(version, context_provenance_record_version);
    ProvenanceRecord decoded{};
    ASSERT_TRUE(read_provenance_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded,
                                       version));
    EXPECT_EQ(decoded.origin.id, context);
    EXPECT_EQ(decoded.origin.generation, original.origin.generation);
    for (uint32_t size = 0; size < bytes.size(); ++size) {
        decoded.id = 99;
        EXPECT_FALSE(read_provenance_record(bytes.data(), size, decoded, version));
        EXPECT_EQ(decoded.id, 99U);
    }
    decoded.id = 99;
    EXPECT_FALSE(
        read_provenance_record(nullptr, static_cast<uint32_t>(bytes.size()), decoded, version));
    constexpr auto future_version = static_cast<uint16_t>(context_provenance_record_version + 1);
    EXPECT_FALSE(read_provenance_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded,
                                        future_version));
    // records.md type24/v2: the appended Context origin starts with its u64 ID.
    const auto origin_at = bytes.size() - provenance_origin_bytes;
    std::fill_n(bytes.data() + origin_at, sizeof(uint64_t), 0);
    EXPECT_FALSE(read_provenance_record(bytes.data(), static_cast<uint32_t>(bytes.size()), decoded,
                                        version));
    EXPECT_EQ(decoded.id, 99U);
}
} // namespace
