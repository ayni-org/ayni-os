// Native composition root (ADR-0010): a Kernel on the native Host, with its
// log in a state directory (MA-010), from assembly through recovery and
// shutdown to release.

#include "temp_directory.h"

#include <ayni/hosts/native.h>
#include <ayni/runtime/native_node.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace {

using ayni::runtime::NativeNode;
using ayni::test::TempDirectory;
using namespace std::chrono_literals;

ayni_lifecycle_t lifecycle_of(const NativeNode &node) {
    ayni_lifecycle_t state = 0;
    EXPECT_EQ(ayni_get_lifecycle(node.kernel(), &state), AYNI_OK);
    return state;
}

// Steps the Kernel while it is in state, waiting for the Host's wake when
// nothing is ready, as an owner thread does.
void step_while(NativeNode &node, ayni_lifecycle_t state) {
    for (int round = 0; round < 200 && lifecycle_of(node) == state; ++round) {
        ayni_step_report_v1 report{};
        report.struct_size = sizeof(report);
        ASSERT_EQ(ayni_step(node.kernel(), 64, 10000, &report), AYNI_OK);
        if (report.more_ready == 0U && lifecycle_of(node) == state) {
            static_cast<void>(node.wait_for_wake(1s));
        }
    }
    EXPECT_NE(lifecycle_of(node), state);
}

// Starts the Kernel and steps it through recovery; returns its boot epoch.
uint64_t boot(NativeNode &node) {
    EXPECT_EQ(ayni_start(node.kernel()), AYNI_OK);
    step_while(node, AYNI_LIFECYCLE_RECOVERING);
    ayni_kernel_info_v1 info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(ayni_get_kernel_info(node.kernel(), &info), AYNI_OK);
    return info.boot_epoch;
}

void shut_down(NativeNode &node) {
    ASSERT_EQ(ayni_begin_shutdown(node.kernel()), AYNI_OK);
    step_while(node, AYNI_LIFECYCLE_DRAINING);
    EXPECT_EQ(lifecycle_of(node), AYNI_LIFECYCLE_STOPPED);
}

// One boot of a node on directory, from assembly to release.
uint64_t boot_once(const TempDirectory &directory) {
    NativeNode node;
    EXPECT_EQ(node.assemble(directory.path().c_str()), AYNI_OK);
    const uint64_t epoch = boot(node);
    EXPECT_EQ(lifecycle_of(node), AYNI_LIFECYCLE_RUNNING);
    shut_down(node);
    EXPECT_EQ(node.release(), AYNI_OK);
    return epoch;
}

TEST(NativeNode, assemble_creates_configuring_kernel) {
    const TempDirectory directory;
    NativeNode node;
    ASSERT_EQ(node.assemble(directory.path().c_str()), AYNI_OK);
    ASSERT_NE(node.kernel(), nullptr);
    EXPECT_EQ(lifecycle_of(node), AYNI_LIFECYCLE_CONFIGURING);
    EXPECT_EQ(node.release(), AYNI_OK);
    EXPECT_EQ(node.kernel(), nullptr);
}

TEST(NativeNode, kernel_recovers_and_stops_on_the_file_log) {
    const TempDirectory directory;
    NativeNode node;
    ASSERT_EQ(node.assemble(directory.path().c_str()), AYNI_OK);
    EXPECT_EQ(boot(node), 1U);
    EXPECT_EQ(lifecycle_of(node), AYNI_LIFECYCLE_RUNNING);
    shut_down(node);
    EXPECT_EQ(node.release(), AYNI_OK);
    EXPECT_TRUE(std::filesystem::exists(directory.file(ayni::hosts::native::Host::log_file_name)));
}

TEST(NativeNode, each_boot_on_the_same_directory_persists_the_next_epoch) {
    const TempDirectory directory;
    for (uint64_t expected = 1; expected <= 3; ++expected) {
        EXPECT_EQ(boot_once(directory), expected);
    }
}

// A power cut before the second boot's epoch reached the medium: the next
// boot drops what is left of that batch and writes epoch 2 again.
TEST(NativeNode, torn_boot_batch_is_written_again_on_the_next_boot) {
    const TempDirectory directory;
    ASSERT_EQ(boot_once(directory), 1U);
    ASSERT_EQ(boot_once(directory), 2U);
    const std::string log = directory.file(ayni::hosts::native::Host::log_file_name);
    std::filesystem::resize_file(log, std::filesystem::file_size(log) - 1U);
    EXPECT_EQ(boot_once(directory), 2U);
    EXPECT_EQ(boot_once(directory), 3U);
}

// C-HOST-018: the Host's thread may stop only once nothing is in flight.
TEST(NativeNode, release_waits_for_a_stopped_kernel) {
    const TempDirectory directory;
    NativeNode node;
    ASSERT_EQ(node.assemble(directory.path().c_str()), AYNI_OK);
    ASSERT_EQ(boot(node), 1U);
    ayni_kernel_t *const kernel = node.kernel();
    EXPECT_EQ(node.release(), AYNI_INVALID_STATE);
    EXPECT_EQ(node.kernel(), kernel);
    EXPECT_EQ(lifecycle_of(node), AYNI_LIFECYCLE_RUNNING);
    shut_down(node);
    EXPECT_EQ(node.release(), AYNI_OK);
}

TEST(NativeNode, second_node_on_the_same_directory_is_busy) {
    const TempDirectory directory;
    NativeNode first;
    ASSERT_EQ(first.assemble(directory.path().c_str()), AYNI_OK);
    NativeNode second;
    EXPECT_EQ(second.assemble(directory.path().c_str()), AYNI_BUSY);
    EXPECT_EQ(second.kernel(), nullptr);
    ASSERT_EQ(first.release(), AYNI_OK);
    EXPECT_EQ(second.assemble(directory.path().c_str()), AYNI_OK);
}

TEST(NativeNode, assemble_reports_why_the_log_cannot_open) {
    const TempDirectory directory;
    NativeNode node;
    EXPECT_EQ(node.assemble(nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(node.assemble(directory.file("missing").c_str()), AYNI_STORAGE_FAILED);
    {
        std::ofstream other(directory.file(ayni::hosts::native::Host::log_file_name));
        other << "not a log at all";
    }
    EXPECT_EQ(node.assemble(directory.path().c_str()), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(node.kernel(), nullptr);
}

TEST(NativeNode, second_assemble_is_invalid_state) {
    const TempDirectory directory;
    NativeNode node;
    ASSERT_EQ(node.assemble(directory.path().c_str()), AYNI_OK);
    ayni_kernel_t *const first = node.kernel();
    EXPECT_EQ(node.assemble(directory.path().c_str()), AYNI_INVALID_STATE);
    EXPECT_EQ(node.kernel(), first);
}

TEST(NativeNode, release_without_kernel_is_invalid_state) {
    NativeNode node;
    EXPECT_EQ(node.release(), AYNI_INVALID_STATE);
    EXPECT_FALSE(node.wait_for_wake(1ms));
}

TEST(NativeNode, assemble_after_release_creates_a_new_kernel) {
    const TempDirectory directory;
    NativeNode node;
    for (int round = 0; round < 3; ++round) {
        ASSERT_EQ(node.assemble(directory.path().c_str()), AYNI_OK);
        ASSERT_EQ(node.release(), AYNI_OK);
    }
}

TEST(NativeNode, destructor_releases_assembled_kernel) {
    // Leak detection (ASan in CI) proves the memory and the Host are returned.
    const TempDirectory directory;
    NativeNode node;
    ASSERT_EQ(node.assemble(directory.path().c_str()), AYNI_OK);
}

} // namespace
