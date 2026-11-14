// White-box ADR-0102: ESP's exclusive volume ledger is a lower bound, never a reclaimed-space
// guess.
#include "../../hosts/esp_idf/space_ledger.h"
#include <gtest/gtest.h>
namespace {
using ayni::hosts::esp_idf::SpaceLedger;
TEST(EspSpaceLedger, repeated_writes_charge_one_block_and_reads_keep_the_estimate) {
    SpaceLedger ledger;
    uint64_t bytes = 0;
    EXPECT_FALSE(ledger.available(bytes));
    constexpr uint64_t initial = 1024U * 1024U;
    ledger.measured(initial);
    ledger.changing(7);
    ledger.changing(7);
    ASSERT_TRUE(ledger.available(bytes));
    EXPECT_EQ(bytes, initial - SpaceLedger::block_bytes);
    EXPECT_FALSE(ledger.needs_measurement());
    ASSERT_TRUE(ledger.available(bytes));
    EXPECT_EQ(bytes, initial - SpaceLedger::block_bytes);
}
TEST(EspSpaceLedger, reclaim_is_credited_only_by_measurement_and_zero_is_a_valid_budget) {
    SpaceLedger ledger;
    uint64_t bytes = 0;
    ledger.measured(SpaceLedger::block_bytes);
    EXPECT_FALSE(ledger.needs_measurement());
    ledger.changing(1);
    ASSERT_TRUE(ledger.available(bytes));
    EXPECT_EQ(bytes, 0U);
    EXPECT_TRUE(ledger.needs_measurement());
    ledger.measured(0);
    ASSERT_TRUE(ledger.available(bytes));
    EXPECT_EQ(bytes, 0U);
    EXPECT_FALSE(ledger.needs_measurement());
    ledger.measured(SpaceLedger::reconciliation_bytes);
    ASSERT_TRUE(ledger.available(bytes));
    EXPECT_EQ(bytes, SpaceLedger::reconciliation_bytes);
}
TEST(EspSpaceLedger, accumulated_changes_require_reconciliation_before_tracking_overflow) {
    SpaceLedger ledger;
    ledger.measured(1024U * 1024U);
    constexpr auto debt_blocks = SpaceLedger::reconciliation_bytes / SpaceLedger::block_bytes;
    for (uint32_t at = 0; at < debt_blocks; ++at) {
        ledger.changing(at);
    }
    EXPECT_TRUE(ledger.needs_measurement());
    for (uint32_t at = static_cast<uint32_t>(debt_blocks); at <= SpaceLedger::tracked_blocks;
         ++at) {
        ledger.changing(at);
    }
    uint64_t bytes = 0;
    EXPECT_FALSE(ledger.available(bytes));
    ledger.measured(0);
    ledger.invalidate();
    EXPECT_TRUE(ledger.needs_measurement());
}
} // namespace
