// ADR-0114: pure local shaping is independent of Core and still requires PermitGate admission.
#include <array>
#include <ayni/control_loop.h>
#include <climits>
#include <cstring>
#include <gtest/gtest.h>

namespace {
ayni_catalog_name_v1 loop_name(const char *name) {
    ayni_catalog_name_v1 out{};
    out.size = static_cast<uint32_t>(std::strlen(name));
    std::memcpy(out.data, name, out.size);
    return out;
}
ayni_control_segment_v1 desired(int64_t first = 80, int64_t second = -60) {
    ayni_control_segment_v1 out{};
    out.struct_size = sizeof(out);
    out.field_count = 2;
    out.fields[0] = {loop_name("first"), loop_name("percent"), first};
    out.fields[1] = {loop_name("second"), loop_name("percent"), second};
    return out;
}
ayni_control_loop_t loop_config() {
    ayni_control_loop_t out{};
    out.struct_size = sizeof(out);
    out.field_count = 2;
    out.fields[0] = loop_name("first");
    out.fields[1] = loop_name("second");
    out.sensor_count = 1;
    out.sensors[0] = {1, 0, 1000, 200, 600};
    out.max_cycle_gap_us = 2000;
    return out;
}
TEST(ControlLoop, SensorAndHoldOnlyTightenAndInputStateIsUnchanged) {
    // ADR-0114: 400 mm means 50%; 25% hold is the stricter bound.
    const auto loop = loop_config();
    const auto target = desired();
    const ayni_control_sensor_sample_v1 sensor{1, 1, 100, 400};
    ayni_control_loop_t next{};
    ayni_control_segment_v1 effective{};
    ayni_control_loop_action_t action = 0;
    ASSERT_EQ(
        ayni_control_loop_limit(&loop, &target, &sensor, 1, 100, 25, &effective, &action, &next),
        AYNI_OK);
    EXPECT_EQ(action, AYNI_LOOP_APPLY);
    EXPECT_EQ(effective.fields[0].value, 20);
    EXPECT_EQ(effective.fields[1].value, -15);
    EXPECT_EQ(loop.has_previous, 0U);
    EXPECT_EQ(next.last_step_us, 100U);
    EXPECT_EQ(target.fields[0].value, 80);
    // Adopting explicit next state permits in-place out_next without hidden mutation.
    ASSERT_EQ(
        ayni_control_loop_limit(&next, &target, &sensor, 1, 101, 100, &effective, &action, &next),
        AYNI_OK);
    EXPECT_EQ(effective.fields[0].value, 40);
}
TEST(ControlLoop, MissingStaleDegradedFutureOrStopThresholdRequiresSoftStop) {
    // ADR-0114: every configured source is required; no inferred fallback.
    const auto loop = loop_config();
    const auto target = desired();
    for (const auto sample : {ayni_control_sensor_sample_v1{1, 0, 100, 900},
                              {1, 1, 1101, 900},
                              {1, 1, 99, 900},
                              {1, 1, 100, 200}}) {
        ayni_control_loop_t next{};
        ayni_control_segment_v1 effective{};
        ayni_control_loop_action_t action = 0;
        ASSERT_EQ(ayni_control_loop_limit(&loop, &target, &sample, 1, 1100, 100, &effective,
                                          &action, &next),
                  AYNI_OK);
        EXPECT_EQ(action, AYNI_LOOP_SOFT_STOP);
        EXPECT_EQ(effective.fields[0].value, 0);
    }
    ayni_control_loop_t next{};
    ayni_control_segment_v1 effective{};
    ayni_control_loop_action_t action = 0;
    ASSERT_EQ(
        ayni_control_loop_limit(&loop, &target, nullptr, 0, 100, 100, &effective, &action, &next),
        AYNI_OK);
    EXPECT_EQ(action, AYNI_LOOP_SOFT_STOP);
}
TEST(ControlLoop, CycleWatchdogAndSlewNeverDelayReductionOrReverseOutput) {
    // ADR-0114: increase-only slew cap, immediate reduction, and bounded cycle gaps.
    auto loop = loop_config();
    loop.sensor_count = 0;
    loop.slew_per_step = 5;
    auto target = desired();
    ayni_control_segment_v1 effective{};
    ayni_control_loop_action_t action = 0;
    ASSERT_EQ(
        ayni_control_loop_limit(&loop, &target, nullptr, 0, 100, 25, &effective, &action, &loop),
        AYNI_OK);
    EXPECT_EQ(effective.fields[0].value, 5); // Startup slew starts from zero.
    ASSERT_EQ(
        ayni_control_loop_limit(&loop, &target, nullptr, 0, 200, 100, &effective, &action, &loop),
        AYNI_OK);
    EXPECT_EQ(effective.fields[0].value, 10);
    target = desired(-80, 60);
    ASSERT_EQ(
        ayni_control_loop_limit(&loop, &target, nullptr, 0, 300, 100, &effective, &action, &loop),
        AYNI_OK);
    EXPECT_EQ(effective.fields[0].value, -5);
    ASSERT_EQ(
        ayni_control_loop_limit(&loop, &target, nullptr, 0, 2301, 100, &effective, &action, &loop),
        AYNI_OK);
    EXPECT_EQ(action, AYNI_LOOP_SOFT_STOP);
    EXPECT_EQ(effective.fields[0].value, 0);
}
TEST(ControlLoop, FullIntegerDomainScalingDoesNotOverflow) {
    // ADR-0114: unsigned-magnitude math handles INT64_MIN and extreme sensor bounds.
    auto loop = loop_config();
    loop.sensors[0].stop_value = INT64_MIN;
    loop.sensors[0].slow_value = INT64_MAX;
    const auto target = desired(INT64_MIN, INT64_MAX);
    const ayni_control_sensor_sample_v1 sensor{1, 1, 1, 0};
    ayni_control_loop_t next{};
    ayni_control_segment_v1 effective{};
    ayni_control_loop_action_t action = 0;
    ASSERT_EQ(
        ayni_control_loop_limit(&loop, &target, &sensor, 1, 1, 100, &effective, &action, &next),
        AYNI_OK);
    EXPECT_EQ(effective.fields[0].value, INT64_MIN / 2);
    EXPECT_EQ(effective.fields[1].value, INT64_MAX / 2);
}
} // namespace
