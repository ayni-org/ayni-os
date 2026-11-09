#include "turn_status.h"
#include <gtest/gtest.h>
namespace {
// ADR-0097: an input failure must reopen the frontend, including old error text.
TEST(TurnStatus, InputAndTaskFailuresAreTerminalWhileHeldWorkRemainsVisible) {
    using wifichat::turn_status;
    using wifichat::TurnStatus;
    EXPECT_EQ(turn_status("input error=-4"), TurnStatus::failed);
    EXPECT_EQ(turn_status("turn failed: input storage status=-4; no automatic retry"),
              TurnStatus::failed);
    EXPECT_EQ(turn_status("turn failed: goal creation status=-1; no automatic retry"),
              TurnStatus::failed);
    EXPECT_EQ(turn_status("input refused: not ready/busy/over limit"), TurnStatus::failed);
    EXPECT_EQ(turn_status("reply completed"), TurnStatus::complete);
    EXPECT_EQ(turn_status("task held status=-17 reason=0"), TurnStatus::progress);
    EXPECT_EQ(turn_status("preparing input: creating task"), TurnStatus::progress);
}
} // namespace
