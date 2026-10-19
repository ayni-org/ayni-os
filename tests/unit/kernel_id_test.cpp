// Kernel ids and bounded arrays from foundation (C-ID-001, C-ID-010,
// ADR-0014).

#include "foundation/bounded_array.h"
#include "foundation/kernel_id.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using ayni::foundation::BoundedArray;
using ayni::foundation::IdAllocator;
using ayni::foundation::max_boot_epoch;

TEST(KernelId, allocator_refuses_ids_before_it_starts) {
    IdAllocator ids;
    uint64_t id = 7;
    EXPECT_FALSE(ids.available());
    EXPECT_FALSE(ids.allocate(id));
    EXPECT_EQ(id, 7U);
}

// The epoch fills the high half and the sequence starts at 1, so ids of
// different boots never meet and no id is 0.
TEST(KernelId, ids_carry_the_boot_epoch_and_a_sequence_from_1) {
    IdAllocator ids;
    ids.start(3);
    uint64_t first = 0;
    uint64_t second = 0;
    ASSERT_TRUE(ids.allocate(first));
    ASSERT_TRUE(ids.allocate(second));
    EXPECT_EQ(first, (uint64_t{3} << 32U) | 1U);
    EXPECT_EQ(second, (uint64_t{3} << 32U) | 2U);

    IdAllocator next_boot;
    next_boot.start(4);
    uint64_t later = 0;
    ASSERT_TRUE(next_boot.allocate(later));
    EXPECT_GT(later, second);

    IdAllocator last_boot;
    last_boot.start(max_boot_epoch);
    uint64_t last = 0;
    ASSERT_TRUE(last_boot.allocate(last));
    EXPECT_EQ(last >> 32U, max_boot_epoch);
}

// C-ID-001: the sequence never wraps to reuse an id; it runs out instead.
TEST(KernelId, allocator_runs_out_instead_of_wrapping) {
    IdAllocator ids;
    ids.start(1, 0xFFFFFFFEU);
    uint64_t id = 0;
    ASSERT_TRUE(ids.allocate(id));
    ASSERT_TRUE(ids.allocate(id));
    EXPECT_EQ(id, (uint64_t{1} << 32U) | 0xFFFFFFFFU);
    EXPECT_FALSE(ids.available());
    EXPECT_FALSE(ids.allocate(id));
    EXPECT_EQ(id, (uint64_t{1} << 32U) | 0xFFFFFFFFU);
}

TEST(BoundedArray, fills_to_capacity_and_refuses_more) {
    alignas(uint64_t) std::array<unsigned char, BoundedArray<uint64_t>::storage_size(3)> storage{};
    BoundedArray<uint64_t> array(storage.data(), 3);
    EXPECT_EQ(array.size(), 0U);
    for (uint64_t value = 10; value < 13; ++value) {
        EXPECT_TRUE(array.push_back(value));
    }
    EXPECT_TRUE(array.full());
    EXPECT_FALSE(array.push_back(13));
    EXPECT_EQ(array.size(), 3U);
    EXPECT_EQ(array[2], 12U);
}

TEST(BoundedArray, remove_moves_the_last_element_into_the_gap) {
    alignas(uint64_t) std::array<unsigned char, BoundedArray<uint64_t>::storage_size(3)> storage{};
    BoundedArray<uint64_t> array(storage.data(), 3);
    for (uint64_t value = 10; value < 13; ++value) {
        ASSERT_TRUE(array.push_back(value));
    }
    array.remove_at(0);
    ASSERT_EQ(array.size(), 2U);
    EXPECT_EQ(array[0], 12U);
    EXPECT_EQ(array[1], 11U);
    array.remove_at(1);
    ASSERT_EQ(array.size(), 1U);
    EXPECT_EQ(array[0], 12U);
    EXPECT_TRUE(array.push_back(14));
}

TEST(BoundedArray, zero_capacity_is_always_full) {
    BoundedArray<uint64_t> array(nullptr, 0);
    EXPECT_TRUE(array.full());
    EXPECT_FALSE(array.push_back(1));
}

} // namespace
