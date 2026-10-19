// Native Host services (C-HOST-007, ADR-0015): clock, wake and ingress lock.

#include <ayni/hosts/native.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

TEST(NativeHost, table_is_complete_and_points_at_the_host) {
    ayni::hosts::native::Host native;
    const ayni_host_v1 host = native.table();
    EXPECT_EQ(host.struct_size, sizeof(ayni_host_v1));
    EXPECT_EQ(host.ctx, &native);
    EXPECT_NE(host.monotonic_now_us, nullptr);
    EXPECT_NE(host.wake, nullptr);
    EXPECT_NE(host.ingress_lock, nullptr);
    EXPECT_NE(host.ingress_unlock, nullptr);
}

TEST(NativeHost, monotonic_clock_never_decreases) {
    ayni::hosts::native::Host native;
    const ayni_host_v1 host = native.table();
    uint64_t previous = host.monotonic_now_us(host.ctx);
    for (int sample = 0; sample < 10000; ++sample) {
        const uint64_t now = host.monotonic_now_us(host.ctx);
        ASSERT_GE(now, previous);
        previous = now;
    }
}

TEST(NativeHost, wakes_from_other_threads_merge_into_one) {
    ayni::hosts::native::Host native;
    const ayni_host_v1 host = native.table();
    EXPECT_FALSE(native.wait_for_wake(1ms));

    std::vector<std::thread> wakers;
    for (int thread = 0; thread < 4; ++thread) {
        wakers.emplace_back([&host] { host.wake(host.ctx); });
    }
    for (std::thread &waker : wakers) {
        waker.join();
    }
    EXPECT_TRUE(native.wait_for_wake(1s));
    EXPECT_FALSE(native.wait_for_wake(1ms));
}

TEST(NativeHost, ingress_lock_excludes_other_threads) {
    ayni::hosts::native::Host native;
    const ayni_host_v1 host = native.table();
    uint64_t counter = 0; // unsynchronized except by the ingress lock
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&host, &counter] {
            for (int round = 0; round < 10000; ++round) {
                host.ingress_lock(host.ctx);
                ++counter;
                host.ingress_unlock(host.ctx);
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    EXPECT_EQ(counter, 40000U);
}

} // namespace
