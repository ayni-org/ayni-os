#include "reset_plan.h"
#include <gtest/gtest.h>
namespace {
// ADR-0097: the confirmed rebuild covers only experiment data, never credentials or app.
TEST(ResetPlan, OnlyTheExactAyniVolumeIsEligibleForRebuild) {
    using wifichat::wifi::ResetPlan;
    EXPECT_TRUE(ResetPlan::matches("ayni", 0x800000, 0x800000));
    EXPECT_FALSE(ResetPlan::matches("nvs", 0x406000, 0xa000));
    EXPECT_FALSE(ResetPlan::matches("demo_cfg", 0x400000, 0x6000));
    EXPECT_FALSE(ResetPlan::matches("factory", 0x410000, 0x3f0000));
    EXPECT_FALSE(ResetPlan::matches("audio", 0x100000, 0x300000));
    EXPECT_FALSE(ResetPlan::matches("ayni", 0x406000, 0x800000));
    EXPECT_FALSE(ResetPlan::matches("ayni", 0x800000, 0x1000000));
    EXPECT_EQ(ResetPlan::bytes % ResetPlan::chunk, 0U);
}
} // namespace
