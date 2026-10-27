// ADR-0067: normal ownership/move/drain protocol through public API only.
#include <ayni/ayni.hpp>
#include <ayni/host_authority.h>
#include <ayni/hosts/sim.h>
#include <ayni/runtime/common/default_config.h>
#include <cstring>
#include <gtest/gtest.h>
#include <utility>
namespace {
struct Producers {
    ayni::hosts::sim::Host host;
    enum class Readiness { ready, returning, joined };
    Readiness readiness = Readiness::ready;
    uint32_t joins = 0;
    static ayni_status_t progress(void *ctx) {
        auto &self = *static_cast<Producers *>(ctx);
        self.host.advance_us(1000);
        self.host.complete_storage();
        return AYNI_OK;
    }
    static ayni_status_t quiesce(void *ctx) {
        auto &self = *static_cast<Producers *>(ctx);
        ++self.joins;
        if (self.readiness == Readiness::returning) {
            return AYNI_BUSY;
        }
        self.readiness = Readiness::joined;
        return AYNI_OK;
    }
    void run(ayni::Kernel &kernel) {
        const ayni_principal_desc_v1 owner{sizeof(owner), AYNI_PRINCIPAL_OWNER, 1, 1, 0};
        ASSERT_EQ(ayni_register_principal(kernel.get(), &owner), AYNI_OK);
        ASSERT_EQ(kernel.start(), AYNI_OK);
        for (uint32_t at = 0; at < 100; ++at) {
            ASSERT_EQ(progress(this), AYNI_OK);
            auto report = ayni::initialized<ayni_step_report_v1>();
            ASSERT_EQ(kernel.step(64, 10000, report), AYNI_OK);
            auto info = ayni::initialized<ayni_kernel_info_v1>();
            ASSERT_EQ(ayni_get_kernel_info(kernel.get(), &info), AYNI_OK);
            if (info.lifecycle == AYNI_LIFECYCLE_RUNNING && info.durable_seq > 1) {
                return;
            }
        }
        FAIL() << "normal boot did not become durable";
    }
};
TEST(CppSdk, owning_arena_move_drain_and_producer_join_preserve_one_handle) {
    Producers producers;
    ayni::Kernel original;
    const auto host = producers.host.table();
    ASSERT_EQ(
        original.create(ayni::runtime::default_config(), host, {&producers, Producers::quiesce}),
        AYNI_OK);
    const auto handle = original.get();
    producers.run(original);
    EXPECT_EQ(original.close(), AYNI_INVALID_STATE);
    EXPECT_EQ(producers.joins, 0U);
    ayni::Kernel moved(std::move(original));
    EXPECT_EQ(original.get(), nullptr);
    EXPECT_EQ(moved.get(), handle);
    EXPECT_EQ(moved.drain(0), AYNI_INVALID_ARGUMENT);
    ASSERT_EQ(moved.drain(100, {&producers, Producers::progress}), AYNI_OK);
    producers.readiness = Producers::Readiness::returning;
    EXPECT_EQ(moved.close(), AYNI_BUSY);
    EXPECT_EQ(moved.get(), handle);
    producers.readiness = Producers::Readiness::ready;
    ASSERT_EQ(moved.close(), AYNI_OK);
    EXPECT_EQ(moved.get(), nullptr);
    EXPECT_EQ(moved.close(), AYNI_OK);
}
TEST(CppSdk, caller_arena_remains_owned_by_caller_after_successful_close) {
    Producers producers;
    const auto config = ayni::runtime::default_config();
    size_t bytes = 0, alignment = 0;
    ASSERT_EQ(ayni_kernel_memory_requirements(&config, &bytes, &alignment), AYNI_OK);
    void *memory = ::operator new(bytes, std::align_val_t{alignment}, std::nothrow);
    ASSERT_NE(memory, nullptr);
    {
        ayni::Kernel kernel;
        const auto host = producers.host.table();
        ASSERT_EQ(kernel.create_in(config, host, memory, bytes, {&producers, Producers::quiesce}),
                  AYNI_OK);
        EXPECT_EQ(kernel.create_in(config, host, memory, bytes, {&producers, Producers::quiesce}),
                  AYNI_INVALID_STATE);
        ASSERT_EQ(kernel.close(), AYNI_OK);
    }
    std::memset(memory, 0, bytes);
    ::operator delete(memory, std::align_val_t{alignment});
    EXPECT_EQ(producers.readiness, Producers::Readiness::joined);
}
TEST(CppSdk, configuring_destructor_joins_before_releasing_its_owned_arena) {
    Producers producers;
    {
        ayni::Kernel kernel;
        ASSERT_EQ(kernel.create(ayni::runtime::default_config(), producers.host.table(),
                                {&producers, Producers::quiesce}),
                  AYNI_OK);
    }
    EXPECT_EQ(producers.joins, 1U);
    EXPECT_EQ(producers.readiness, Producers::Readiness::joined);
}
} // namespace
