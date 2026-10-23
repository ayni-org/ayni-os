#include "catalog/capability/field_values.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

// ADR-0034 decision 1: a shared payload reader publishes a complete borrowed
// value and byte count together. White-box: direct internal reader contract.
TEST(FieldValues, failed_payload_reads_preserve_both_outputs_at_every_truncation) {
    const std::array<uint8_t, 21> payload{'c', 'o', 'u', 'n', 't', 1, 0, 0, 0, 0, 0,
                                          0,   0,   2,   0,   0,   0, 0, 0, 0, 0};
    ayni::catalog::FieldValues sentinel{};
    sentinel.kind = AYNI_AUTHORITY_ENUM;
    sentinel.unit = {"before", 6};
    sentinel.minimum = -19;
    sentinel.maximum = 37;
    sentinel.tokens.fill({"sentinel", 8});
    sentinel.token_count = 1;
    auto expect_failure = [&](ayni_bytes_v1 body, uint32_t unit_size) {
        auto value = sentinel;
        uint32_t consumed = 91;
        EXPECT_FALSE(ayni::catalog::read_encoded_values(body, AYNI_AUTHORITY_I64, unit_size, 0,
                                                        value, consumed));
        EXPECT_EQ(consumed, 91U);
        EXPECT_EQ(value.kind, sentinel.kind);
        EXPECT_EQ(value.unit.data, sentinel.unit.data);
        EXPECT_EQ(value.unit.size, sentinel.unit.size);
        EXPECT_EQ(value.minimum, sentinel.minimum);
        EXPECT_EQ(value.maximum, sentinel.maximum);
        EXPECT_EQ(value.token_count, sentinel.token_count);
        for (uint32_t index = 0; index < value.tokens.size(); ++index) {
            EXPECT_EQ(value.tokens[index].data, sentinel.tokens[index].data);
            EXPECT_EQ(value.tokens[index].size, sentinel.tokens[index].size);
        }
    };
    for (uint32_t size = 0; size < payload.size(); ++size) {
        SCOPED_TRACE(size);
        expect_failure({payload.data(), size}, 5);
    }
    expect_failure({nullptr, 21}, 5);
    expect_failure({payload.data(), 21}, 33);
    auto value = sentinel;
    uint32_t consumed = 91;
    ASSERT_TRUE(ayni::catalog::read_encoded_values({payload.data(), 21}, AYNI_AUTHORITY_I64, 5, 0,
                                                   value, consumed));
    EXPECT_EQ(consumed, 21U);
    EXPECT_EQ(value.minimum, 1);
    EXPECT_EQ(value.maximum, 2);
    EXPECT_EQ(value.token_count, 0U);
}

} // namespace
