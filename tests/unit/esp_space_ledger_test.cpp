// White-box ADR-0102: ESP's exclusive volume ledger is a lower bound, never a reclaimed-space
// guess.
#include "../../hosts/esp_idf/space_ledger.h"
#include <gtest/gtest.h>
namespace {
using ayni::hosts::esp_idf::SpaceLedger;
using ayni::hosts::esp_idf::SpaceRefresh;
using Need = SpaceLedger::Reconciliation;
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
    EXPECT_EQ(ledger.reconciliation(), Need::clean);
    ASSERT_TRUE(ledger.available(bytes));
    EXPECT_EQ(bytes, initial - SpaceLedger::block_bytes);
}
TEST(EspSpaceLedger, reclaim_is_credited_only_by_measurement_and_zero_is_a_valid_budget) {
    SpaceLedger ledger;
    uint64_t bytes = 0;
    ledger.measured(SpaceLedger::block_bytes);
    EXPECT_EQ(ledger.reconciliation(), Need::clean);
    ledger.changing(1);
    ASSERT_TRUE(ledger.available(bytes));
    EXPECT_EQ(bytes, 0U);
    EXPECT_EQ(ledger.reconciliation(), Need::urgent);
    ledger.measured(0);
    ASSERT_TRUE(ledger.available(bytes));
    EXPECT_EQ(bytes, 0U);
    EXPECT_EQ(ledger.reconciliation(), Need::clean);
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
    EXPECT_EQ(ledger.reconciliation(), Need::routine);
    for (uint32_t at = static_cast<uint32_t>(debt_blocks); at <= SpaceLedger::tracked_blocks;
         ++at) {
        ledger.changing(at);
    }
    uint64_t bytes = 0;
    EXPECT_FALSE(ledger.available(bytes));
    ledger.measured(0);
    ledger.invalidate();
    EXPECT_EQ(ledger.reconciliation(), Need::urgent);
}
// ADR-0102: deferring a routine scan preserves the conservative lower bound and reservations.
TEST(EspSpaceLedger, sufficient_headroom_defers_scan_until_quiet_and_keeps_reserved_debt) {
    SpaceLedger ledger;
    SpaceRefresh refresh;
    constexpr uint64_t initial = 1024U * 1024U;
    constexpr uint64_t reservation = 64U * 1024U;
    ledger.measured(initial);
    for (uint32_t block = 0; block < SpaceLedger::reconciliation_bytes / SpaceLedger::block_bytes;
         ++block) {
        ledger.changing(block);
    }
    const auto need = ledger.reconciliation(reservation);
    EXPECT_EQ(need, Need::routine);
    EXPECT_FALSE(refresh.measure(need, 10, false));
    EXPECT_TRUE(refresh.pending());
    uint64_t free = 0;
    ASSERT_TRUE(ledger.available(free));
    EXPECT_EQ(free - reservation, initial - SpaceLedger::reconciliation_bytes - reservation);
    EXPECT_TRUE(refresh.measure(need, 20, true));
    EXPECT_FALSE(refresh.pending());
}
// ADR-0102: continuous work cannot defer forever, and low headroom or a full tracker is urgent.
TEST(EspSpaceLedger, busy_reconciliation_has_a_deadline_and_urgent_space_takes_precedence) {
    SpaceLedger ledger;
    SpaceRefresh refresh;
    constexpr uint64_t initial = 1024U * 1024U;
    ledger.measured(initial);
    constexpr auto debt_blocks = SpaceLedger::reconciliation_bytes / SpaceLedger::block_bytes;
    for (uint32_t block = 0; block < debt_blocks; ++block) {
        ledger.changing(block);
    }
    const auto need = ledger.reconciliation();
    EXPECT_EQ(need, Need::routine);
    EXPECT_FALSE(refresh.measure(need, 0, false));
    EXPECT_FALSE(refresh.measure(need, SpaceRefresh::max_deferral_us - 1, false));
    EXPECT_TRUE(refresh.measure(need, SpaceRefresh::max_deferral_us, false));
    EXPECT_FALSE(refresh.pending());

    EXPECT_EQ(ledger.reconciliation(initial - SpaceLedger::reconciliation_bytes), Need::urgent);
    EXPECT_TRUE(refresh.measure(Need::urgent, SpaceRefresh::max_deferral_us + 1, false));
    for (uint32_t block = debt_blocks; block < SpaceLedger::tracked_blocks; ++block) {
        ledger.changing(block);
    }
    EXPECT_EQ(ledger.reconciliation(), Need::urgent);
}
} // namespace
