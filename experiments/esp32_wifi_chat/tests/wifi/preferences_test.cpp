#include "nvs_flash.h"
#include "preferences.h"
#include "settings.h"
#include <cstring>
#include <gtest/gtest.h>
using namespace wifichat::wifi;
// User 2026-11-13: automatic voice is opt-in, persisted, and separate from model tool access.
TEST(WifiPreferences, VoiceSwitchSurvivesReloadWithoutChangingSavedEndpoints) {
    reset_nvs();
    Settings settings;
    std::strcpy(settings.ssid.data(), "test-network");
    std::strcpy(settings.model.data(), "test-model");
    ASSERT_TRUE(settings.save());
    Preferences first;
    ASSERT_TRUE(first.load());
    EXPECT_FALSE(first.voice_reply());
    ASSERT_TRUE(first.set_voice_reply(true));
    Preferences reopened;
    ASSERT_TRUE(reopened.load());
    EXPECT_TRUE(reopened.voice_reply());
    Settings endpoints;
    ASSERT_TRUE(endpoints.load());
    EXPECT_STREQ(endpoints.ssid.data(), "test-network");
    EXPECT_STREQ(endpoints.model.data(), "test-model");
    ASSERT_TRUE(reopened.set_voice_reply(false));
    Preferences disabled;
    ASSERT_TRUE(disabled.load());
    EXPECT_FALSE(disabled.voice_reply());
}
