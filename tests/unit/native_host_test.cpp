#include <ayni/hosts/native.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

TEST(NativeHost, table_is_complete_and_needs_no_context) {
    const ayni_host_v1 host = ayni::hosts::native::host_table();
    EXPECT_EQ(host.struct_size, sizeof(ayni_host_v1));
    EXPECT_EQ(host.ctx, nullptr);
    EXPECT_NE(host.monotonic_now_us, nullptr);
}

TEST(NativeHost, monotonic_clock_never_decreases) {
    const ayni_host_v1 host = ayni::hosts::native::host_table();
    uint64_t previous = host.monotonic_now_us(host.ctx);
    for (int sample = 0; sample < 10000; ++sample) {
        const uint64_t now = host.monotonic_now_us(host.ctx);
        ASSERT_GE(now, previous);
        previous = now;
    }
}

} // namespace
