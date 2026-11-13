#include "app_menu.h"
#include "motor_admission.h"
#include "motor_switch.h"
#include <gtest/gtest.h>
using namespace wifichat::wifi;
// User 2026-11-13: visible app tiles and touch targets share one layout.
TEST(WifiApps, EachVisibleIconOpensItsOwnEntryAndGapsAreInactive) {
    for (size_t at = 0; at < app_tiles.size(); ++at) {
        const int x = 4 + int(at % app_columns) * app_pitch_x;
        const int y = app_top + int(at / app_columns) * app_pitch_y;
        ASSERT_TRUE(app_at(x + 36, y + 30));
        EXPECT_EQ(*app_at(x + 36, y + 30), app_tiles[at].app);
        EXPECT_FALSE(app_at(x + app_tile_width, y + 30));
        EXPECT_FALSE(app_at(x + 36, y + app_tile_height));
    }
    EXPECT_FALSE(app_at(0, 0));
    EXPECT_FALSE(app_at(239, 319));
}
// User 2026-11-13: resources prepare automatically, default on, ordinary disable can be undone.
TEST(WifiMotorSwitch, DefaultEnableAndIdleDisableRequireObservedAcknowledgement) {
    MotorSwitch state;
    state.initialized(true);
    EXPECT_TRUE(state.desired());
    EXPECT_EQ(state.state(), MotorSwitch::State::disabled);
    state.queued();
    EXPECT_TRUE(state.active());
    state.observe(1);
    EXPECT_EQ(state.state(), MotorSwitch::State::enabled);
    state.disable();
    EXPECT_FALSE(state.desired());
    EXPECT_FALSE(state.active());
    state.observe(1);
    EXPECT_EQ(state.state(), MotorSwitch::State::disabling);
    state.observe(0);
    EXPECT_EQ(state.state(), MotorSwitch::State::disabled);
    state.enable();
    state.queued();
    state.observe(1);
    EXPECT_EQ(state.state(), MotorSwitch::State::enabled);
    state.stop();
    state.observe(0);
    state.enable();
    EXPECT_EQ(state.state(), MotorSwitch::State::stopped);
    EXPECT_FALSE(state.desired());
}
// Ordinary disarm invalidates every older challenge, even after a later UI enable.
TEST(WifiMotorSwitch, DisableRejectsOldNonceAndAllowsFreshChallenge) {
    wifichat::MotorAdmission admission;
    admission.issued(1);
    EXPECT_TRUE(admission.permits(1));
    admission.disable();
    EXPECT_FALSE(admission.permits(1));
    admission.issued(2);
    EXPECT_TRUE(admission.permits(2));
    EXPECT_FALSE(admission.permits(1));
    admission.disable();
    EXPECT_FALSE(admission.permits(2));
}
