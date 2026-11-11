#include "model_codec.h"
#include "network_time.h"
#include <gtest/gtest.h>
using namespace wifichat;
using namespace wifichat::wifi;
// ADR-0097: cold-boot HTTPS needs system time; LAN HTTP speech/maintenance does not.
TEST(WifiClock, ColdBootBlocksHttpsWithoutBlockingLanHttp) {
    EXPECT_FALSE(request_clock_ready("https://model.example/v1", 0));
    EXPECT_FALSE(
        request_clock_ready("https://model.example/v1", minimum_network_epoch_seconds - 1));
    EXPECT_TRUE(request_clock_ready("https://model.example/v1", minimum_network_epoch_seconds));
    EXPECT_TRUE(request_clock_ready("http://192.168.1.2:8000", 0));
    EXPECT_FALSE(network_clock_ready(0));
}
// C-PROV-004: an explicit local refusal carries a complete known result with no remote effect.
TEST(WifiClock, UnsentModelRefusalPreservesKnownStatusAndNoEffect) {
    Packet packet;
    model_failure(packet, AYNI_NOT_READY, AYNI_REASON_NONE, AYNI_CALL_EFFECT_NONE);
    ASSERT_EQ(packet.kind, Kind::model_error);
    ASSERT_EQ(packet.size, model_result_header_bytes);
    const auto result = read_model_result_header(packet.body.data());
    EXPECT_EQ(result.outcome, AYNI_CALL_FAILED);
    EXPECT_EQ(result.status, AYNI_NOT_READY);
    EXPECT_EQ(result.effect, AYNI_CALL_EFFECT_NONE);
    EXPECT_NE(result.status, AYNI_RESULT_UNKNOWN);
}
