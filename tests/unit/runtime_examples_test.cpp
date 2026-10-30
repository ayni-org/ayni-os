// C-SDK-006 / ADR-0087: extension callbacks share the selected Host's clock.
#include "temp_directory.h"
#include <ayni/runtime/examples.h>
namespace {
struct ExtensionClock {
    ayni::runtime::examples::Fixture *fixture;
    uint64_t last = 0;
    uint32_t advances = 0;
    static ayni_status_t advance(void *context, uint64_t now) {
        auto &self = *static_cast<ExtensionClock *>(context);
        EXPECT_EQ(now, self.fixture->now_us());
        EXPECT_GT(now, 0U);
        EXPECT_GE(now, self.last);
        self.last = now;
        ++self.advances;
        return AYNI_OK;
    }
};
TEST(SdkHosts, native_extension_uses_selected_monotonic_clock) {
    ayni::test::TempDirectory directory;
    ayni::runtime::examples::Fixture fixture;
    ExtensionClock clock{&fixture};
    const ayni::runtime::examples::HostConfig host{
        ayni::runtime::examples::HostConfig::Kind::native, directory.path().c_str()};
    ASSERT_EQ(fixture.open({&clock, nullptr, ExtensionClock::advance, nullptr},
                           ayni::runtime::examples::Model::mock,
                           ayni::runtime::examples::Software::none, host),
              AYNI_OK);
    EXPECT_EQ(fixture.step(), AYNI_OK);
    EXPECT_GT(clock.advances, 0U);
    EXPECT_EQ(fixture.close(), AYNI_OK);
}
} // namespace
