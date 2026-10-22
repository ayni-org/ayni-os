// The owner binding is a required, portable singleton record. Its reserved
// bytes cannot carry unknown ownership semantics, and a bad body publishes
// no partially decoded identity.

#include "authority/principal/owner_binding_records.h"
#include "authority/principal/principal_registry.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace {

using ayni::authority::owner_binding_record_size;
using ayni::authority::OwnerBinding;
using ayni::authority::OwnerBindingRecord;
using ayni::authority::read_owner_binding_record;
using ayni::authority::write_owner_binding_record;

TEST(OwnerBindingRecords, writes_fixed_width_little_endian_fields_and_zero_reserved_bytes) {
    const OwnerBinding binding{0x0807060504030201ULL, 0x0C0B0A09U, {17, 19}};
    std::array<uint8_t, owner_binding_record_size + 1U> body{};
    body.fill(0xFF);
    write_owner_binding_record(binding, body.data());

    const std::array<uint8_t, owner_binding_record_size> expected{1, 2,  3,  4,  5, 6, 7, 8,
                                                                  9, 10, 11, 12, 0, 0, 0, 0};
    for (uint32_t index = 0; index < owner_binding_record_size; ++index) {
        EXPECT_EQ(body[index], expected[index]) << index;
    }
    EXPECT_EQ(body.back(), 0xFFU);
    OwnerBindingRecord record{};
    ASSERT_TRUE(read_owner_binding_record(body.data(), owner_binding_record_size, record));
    EXPECT_EQ(record.principal_id, binding.principal_id);
    EXPECT_EQ(record.binding_revision, binding.binding_revision);
}

TEST(OwnerBindingRecords, body_does_not_include_or_depend_on_the_transition_stamp) {
    std::array<uint8_t, owner_binding_record_size> first{};
    std::array<uint8_t, owner_binding_record_size> second{};
    write_owner_binding_record({7, 3, {2, 4}}, first.data());
    write_owner_binding_record({7, 3, {11, 13}}, second.data());
    EXPECT_EQ(first, second);
}

TEST(OwnerBindingRecords, accepts_the_largest_nonzero_identity_and_binding_revision) {
    const OwnerBinding binding{
        std::numeric_limits<uint64_t>::max(), std::numeric_limits<uint32_t>::max(), {2, 4}};
    std::array<uint8_t, owner_binding_record_size> body{};
    write_owner_binding_record(binding, body.data());
    OwnerBindingRecord record{};
    ASSERT_TRUE(read_owner_binding_record(body.data(), body.size(), record));
    EXPECT_EQ(record.principal_id, binding.principal_id);
    EXPECT_EQ(record.binding_revision, binding.binding_revision);
}

TEST(OwnerBindingRecords,
     refuses_every_truncated_body_and_a_trailing_byte_without_changing_output) {
    std::array<uint8_t, owner_binding_record_size + 1U> body{};
    write_owner_binding_record({7, 3, {2, 4}}, body.data());
    for (uint32_t size = 0; size <= body.size(); ++size) {
        if (size == owner_binding_record_size) {
            continue;
        }
        OwnerBindingRecord record{29, 31};
        EXPECT_FALSE(read_owner_binding_record(body.data(), size, record)) << size;
        EXPECT_EQ(record.principal_id, 29U) << size;
        EXPECT_EQ(record.binding_revision, 31U) << size;
    }
}

TEST(OwnerBindingRecords, refuses_a_null_body_without_changing_output) {
    OwnerBindingRecord record{29, 31};
    EXPECT_FALSE(read_owner_binding_record(nullptr, owner_binding_record_size, record));
    EXPECT_EQ(record.principal_id, 29U);
    EXPECT_EQ(record.binding_revision, 31U);
}

TEST(OwnerBindingRecords, refuses_zero_identity_or_binding_revision_without_changing_output) {
    for (const OwnerBinding binding : {OwnerBinding{0, 3, {2, 4}}, OwnerBinding{7, 0, {2, 4}}}) {
        std::array<uint8_t, owner_binding_record_size> body{};
        write_owner_binding_record(binding, body.data());
        OwnerBindingRecord record{29, 31};
        EXPECT_FALSE(read_owner_binding_record(body.data(), body.size(), record));
        EXPECT_EQ(record.principal_id, 29U);
        EXPECT_EQ(record.binding_revision, 31U);
    }
}

TEST(OwnerBindingRecords, refuses_each_nonzero_reserved_byte_without_changing_output) {
    for (uint32_t offset = 12; offset < owner_binding_record_size; ++offset) {
        std::array<uint8_t, owner_binding_record_size> body{};
        write_owner_binding_record({7, 3, {2, 4}}, body.data());
        body[offset] = 1;
        OwnerBindingRecord record{29, 31};
        EXPECT_FALSE(read_owner_binding_record(body.data(), body.size(), record)) << offset;
        EXPECT_EQ(record.principal_id, 29U);
        EXPECT_EQ(record.binding_revision, 31U);
    }
}

} // namespace
