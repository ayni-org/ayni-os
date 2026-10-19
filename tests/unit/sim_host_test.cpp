// Simulation Host (MA-003): FakeClock, counted wakes and lock misuse
// detection used by the kernel tests.

#include <ayni/hosts/sim.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

TEST(SimHost, fake_clock_moves_only_when_advanced) {
    ayni::hosts::sim::Host sim(1000);
    const ayni_host_v1 host = sim.table();
    EXPECT_EQ(host.monotonic_now_us(host.ctx), 1000U);
    EXPECT_EQ(host.monotonic_now_us(host.ctx), 1000U);
    sim.advance_us(250);
    EXPECT_EQ(host.monotonic_now_us(host.ctx), 1250U);
    EXPECT_EQ(sim.now_us(), 1250U);
}

TEST(SimHost, wakes_are_counted) {
    ayni::hosts::sim::Host sim;
    const ayni_host_v1 host = sim.table();
    host.wake(host.ctx);
    host.wake(host.ctx);
    EXPECT_EQ(sim.wake_count(), 2U);
}

TEST(SimHost, host_calls_under_the_ingress_lock_are_recorded) {
    ayni::hosts::sim::Host sim;
    const ayni_host_v1 host = sim.table();
    host.wake(host.ctx);
    EXPECT_EQ(sim.calls_under_lock(), 0U);
    host.ingress_lock(host.ctx);
    host.wake(host.ctx);
    static_cast<void>(host.monotonic_now_us(host.ctx));
    host.ingress_unlock(host.ctx);
    EXPECT_EQ(sim.calls_under_lock(), 2U);
}

// The ingress lock is not reentrant (C-HOST-020); the sim Host turns the
// deadlock a second lock would cause into an immediate abort.
TEST(SimHostDeathTest, second_ingress_lock_on_one_thread_aborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ayni::hosts::sim::Host sim;
    const ayni_host_v1 host = sim.table();
    EXPECT_DEATH(
        {
            host.ingress_lock(host.ctx);
            host.ingress_lock(host.ctx);
        },
        "ingress lock taken twice");
}

} // namespace
