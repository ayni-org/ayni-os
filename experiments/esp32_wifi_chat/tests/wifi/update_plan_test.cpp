#include "update_plan.h"
#include <gtest/gtest.h>
using wifichat::wifi::UpdatePlan;
using Phase = UpdatePlan::Phase;
// ADR-0099: no flash reception before confirmed shutdown, or selection before complete validation.
TEST(WifiUpdate, NormalUploadRequiresShutdownValidationAndReplyBarriers) {
    UpdatePlan plan;
    EXPECT_FALSE(plan.request(0, 100));
    EXPECT_FALSE(plan.request(101, 100));
    ASSERT_TRUE(plan.request(100, 100));
    EXPECT_FALSE(plan.request(1, 100));
    EXPECT_FALSE(plan.append(100));
    ASSERT_TRUE(plan.close());
    EXPECT_FALSE(plan.append(100));
    plan.stopped();
    ASSERT_TRUE(plan.append(40));
    EXPECT_FALSE(plan.verify());
    EXPECT_FALSE(plan.append(61));
    ASSERT_TRUE(plan.append(60));
    EXPECT_FALSE(plan.commit());
    ASSERT_TRUE(plan.verify());
    ASSERT_TRUE(plan.commit());
    EXPECT_FALSE(plan.reboot_due(10000));
    plan.replied(10000);
    EXPECT_FALSE(plan.reboot_due(10999));
    EXPECT_TRUE(plan.reboot_due(11000));
}
// ADR-0099: UI rejection before shutdown keeps the session alive and allows a later explicit
// request.
TEST(WifiUpdate, RefusedShutdownDoesNotRebootOrAdvanceToFlash) {
    UpdatePlan plan;
    ASSERT_TRUE(plan.request(100, 100));
    ASSERT_TRUE(plan.close());
    plan.close_refused(7);
    plan.stopped();
    EXPECT_EQ(plan.snapshot().phase, Phase::rejected);
    EXPECT_EQ(plan.snapshot().error, 7);
    plan.replied(1);
    EXPECT_FALSE(plan.reboot_due(10000));
    EXPECT_TRUE(plan.request(100, 100));
}
// ADR-0099: a reply timeout cannot reboot while Core/storage shutdown is still pending.
TEST(WifiUpdate, IncompleteCloseWaitsForStoppedBeforeRestoringRuntime) {
    UpdatePlan plan;
    ASSERT_TRUE(plan.request(100, 100));
    ASSERT_TRUE(plan.close());
    plan.fail(8);
    plan.replied(1);
    EXPECT_FALSE(plan.reboot_due(10000));
    EXPECT_EQ(plan.snapshot().phase, Phase::aborting_close);
    plan.stopped();
    EXPECT_EQ(plan.snapshot().phase, Phase::restore);
    EXPECT_TRUE(plan.reboot_due(10000));
    EXPECT_FALSE(plan.commit());
}
