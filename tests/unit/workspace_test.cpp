// ADR-0090 / C-SDK-007: temporary-memory admission preserves public outputs,
// independent owners can run concurrently, and rejected callbacks release private bytes.
#include <algorithm>
#include <array>
#include <ayni/arguments.h>
#include <ayni/detail/workspace.hpp>
#include <ayni/workspace.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <utility>

namespace {
struct Scalar {
    ayni_schema_node_v1 schema{};
    ayni_value_node_v1 value{}, output{};
    ayni_schema_v1 declaration{&schema, 1};
    ayni_value_v1 input{&value, 1};
    ayni_value_buffer_v1 result{&output, 1, 0, nullptr, 0, 0};
    explicit Scalar(int64_t number) {
        schema.kind = value.kind = AYNI_VALUE_I64;
        schema.subtree_end = value.subtree_end = 1;
        schema.min_i64 = INT64_MIN;
        schema.max_i64 = INT64_MAX;
        value.value.integer = number;
    }
};
TEST(Workspace, queries_reject_missing_outputs_without_changing_the_other_output) {
    for (auto query : {ayni_value_workspace_requirements, ayni_business_workspace_requirements}) {
        size_t bytes = 17, alignment = 19;
        EXPECT_EQ(query(nullptr, &alignment), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(alignment, 19U);
        EXPECT_EQ(query(&bytes, nullptr), AYNI_INVALID_ARGUMENT);
        EXPECT_EQ(bytes, 17U);
        ASSERT_EQ(query(&bytes, &alignment), AYNI_OK);
        EXPECT_GT(bytes, 0U);
        EXPECT_EQ(alignment & (alignment - 1), 0U);
    }
}
TEST(Workspace, undersized_misaligned_or_unknown_regions_preserve_value_outputs) {
    ayni::Workspace memory;
    ASSERT_EQ(memory.create(ayni::Workspace::Purpose::value), AYNI_OK);
    Scalar scalar(42);
    scalar.output.kind = AYNI_VALUE_BOOL;
    scalar.result.node_count = 7;
    auto region = *memory.get();
    --region.byte_capacity;
    EXPECT_EQ(ayni_value_normalize(&region, &scalar.declaration, &scalar.input, &scalar.result),
              AYNI_BUFFER_TOO_SMALL);
    region = *memory.get();
    region.data = static_cast<uint8_t *>(region.data) + 1;
    EXPECT_EQ(ayni_value_normalize(&region, &scalar.declaration, &scalar.input, &scalar.result),
              AYNI_INVALID_ARGUMENT);
    region = *memory.get();
    ++region.struct_size;
    EXPECT_EQ(ayni_value_normalize(&region, &scalar.declaration, &scalar.input, &scalar.result),
              AYNI_UNSUPPORTED_VERSION);
    region = *memory.get();
    --region.struct_size;
    EXPECT_EQ(ayni_value_normalize(&region, &scalar.declaration, &scalar.input, &scalar.result),
              AYNI_INVALID_ARGUMENT);
    region = *memory.get();
    region.reserved = 1;
    EXPECT_EQ(ayni_schema_validate(&region, &scalar.declaration), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(scalar.output.kind, AYNI_VALUE_BOOL);
    EXPECT_EQ(scalar.result.node_count, 7U);
    ASSERT_EQ(
        ayni_value_normalize(memory.get(), &scalar.declaration, &scalar.input, &scalar.result),
        AYNI_OK);
    EXPECT_EQ(scalar.output.value.integer, 42);
}
TEST(Workspace, independent_regions_normalize_values_on_independent_threads) {
    struct Worker {
        ayni::Workspace memory;
        Scalar scalar;
        ayni_status_t status = AYNI_NOT_READY;
        explicit Worker(int64_t value) : scalar(value) {}
        void run() {
            status = ayni_value_normalize(memory.get(), &scalar.declaration, &scalar.input,
                                          &scalar.result);
        }
    } first(31), second(-59);
    ASSERT_EQ(first.memory.create(ayni::Workspace::Purpose::value), AYNI_OK);
    ASSERT_EQ(second.memory.create(ayni::Workspace::Purpose::value), AYNI_OK);
    EXPECT_NE(first.memory.get()->data, second.memory.get()->data);
    std::thread a([&] { first.run(); }), b([&] { second.run(); });
    a.join();
    b.join();
    EXPECT_EQ(first.status, AYNI_OK);
    EXPECT_EQ(second.status, AYNI_OK);
    EXPECT_EQ(first.scalar.output.value.integer, 31);
    EXPECT_EQ(second.scalar.output.value.integer, -59);
}
TEST(Workspace, move_transfers_ownership_and_borrowed_storage_survives_close) {
    ayni::Workspace memory;
    ASSERT_EQ(memory.create(ayni::Workspace::Purpose::value), AYNI_OK);
    auto *data = memory.get()->data;
    ayni::Workspace moved(std::move(memory));
    EXPECT_EQ(memory.get(), nullptr);
    ASSERT_NE(moved.get(), nullptr);
    EXPECT_EQ(moved.get()->data, data);
    EXPECT_EQ(moved.create(ayni::Workspace::Purpose::value), AYNI_INVALID_STATE);
    alignas(uint64_t) std::array<uint8_t, 32> bytes{};
    ayni::Workspace borrowed;
    ASSERT_EQ(borrowed.create_in(bytes.data(), bytes.size(), alignof(uint64_t)), AYNI_OK);
    borrowed.close();
    bytes.back() = 1;
    EXPECT_EQ(bytes.back(), 1U);
}
TEST(Workspace, callback_refusal_erases_its_lane_and_allows_immediate_reuse) {
    alignas(uint64_t) std::array<uint8_t, 37> bytes{};
    ayni::detail::Workspace workspace(bytes.data(), bytes.size());
    const auto query = +[](size_t *size, size_t *alignment) {
        *size = 37;
        *alignment = alignof(uint64_t);
        return AYNI_OK;
    };
    const auto status =
        ayni::detail::with_workspace(workspace, query, [](const ayni_workspace_v1 *region) {
            auto *data = static_cast<uint8_t *>(region->data);
            std::fill_n(data, region->byte_capacity, uint8_t{73});
            return AYNI_DENIED;
        });
    EXPECT_EQ(status, AYNI_DENIED);
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](uint8_t byte) { return byte == 0; }));
    auto lane = workspace.take(bytes.size());
    EXPECT_TRUE(lane);
}
} // namespace
