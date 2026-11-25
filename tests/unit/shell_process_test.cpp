// Normal shell lifecycle under the user's 2026-10-29 explicit authorization.
// White-box: the native process owner is tested separately from Core admission.
#include "../../providers/shell/process.h"
#include "temp_directory.h"
#include <ayni/providers/shell.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {
using ayni::providers::shell::encode_output;
using ayni::providers::shell::execute;
using ayni::providers::shell::ProcessConfig;
using ayni::providers::shell::ProcessResult;

ProcessConfig configuration() {
    ProcessConfig config;
    config.directory = "/tmp";
    // Headroom for real /bin/sh + pipeline forks under parallel sanitizer CI load;
    // timeout_terminates_sleeping_pipeline overrides this to 30 ms explicitly.
    config.timeout_ms = 5000;
    config.output_bytes = 4096;
    config.environment_view.push_back(nullptr);
    return config;
}

// Shell contract: pipelines/scripts, distinct streams and nonzero exit status
// are real results; a command exiting does not assert its business goal succeeded.
TEST(ShellProcess, executes_pipeline_in_configured_directory_and_preserves_exit_and_streams) {
    auto config = configuration();
    std::atomic<bool> cancel{false};
    const auto result = execute(
        config, "pwd; printf hello | /usr/bin/tr a-z A-Z; printf error >&2; exit 7", cancel);
    ASSERT_EQ(result.end, ProcessResult::End::exited);
    EXPECT_TRUE(result.started);
    EXPECT_EQ(result.exit_code, 7);
    EXPECT_EQ(result.signal, 0);
    EXPECT_EQ(result.standard_output, "/tmp\nHELLO");
    EXPECT_EQ(result.standard_error, "error");
}

// Shell contract: raw output remains lossless, including non-UTF-8 and NUL;
// the business result explicitly labels base64 instead of replacing bytes.
TEST(ShellProcess, binary_output_has_explicit_lossless_encoding) {
    auto config = configuration();
    std::atomic<bool> cancel{false};
    const auto result = execute(config, "printf '\\377\\000'", cancel);
    ASSERT_EQ(result.end, ProcessResult::End::exited);
    EXPECT_EQ(result.standard_output, std::string("\xff\0", 2));
    EXPECT_EQ(encode_output(result.standard_output), "/wA=");
}

// Shell contract: timeout ends tracking of this process group and reaps its
// leader; no retry or fabricated successful command result is made.
TEST(ShellProcess, timeout_terminates_sleeping_pipeline) {
    auto config = configuration();
    config.timeout_ms = 30;
    std::atomic<bool> cancel{false};
    const auto started = std::chrono::steady_clock::now();
    const auto result = execute(config, "/bin/sleep 10 | /bin/cat", cancel);
    EXPECT_EQ(result.end, ProcessResult::End::timeout);
    EXPECT_TRUE(result.started);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
}

// Shell contract: combined stdout/stderr capacity is explicit; the result says
// output_limit rather than silently claiming complete output or success.
TEST(ShellProcess, output_capacity_is_combined_and_truncation_is_explicit) {
    auto config = configuration();
    config.output_bytes = 16;
    std::atomic<bool> cancel{false};
    const auto result = execute(config, "printf 0123456789; printf 0123456789 >&2", cancel);
    EXPECT_EQ(result.end, ProcessResult::End::output_limit);
    EXPECT_EQ(result.standard_output.size() + result.standard_error.size(), 16U);
}

// Shell contract: cancellation before execution proves no process began.
TEST(ShellProcess, pre_cancelled_request_never_starts_process) {
    auto config = configuration();
    std::atomic<bool> cancel{true};
    const auto result = execute(config, "exit 0", cancel);
    EXPECT_EQ(result.end, ProcessResult::End::cancelled);
    EXPECT_FALSE(result.started);
}

// Shell contract: the whole execution authority must be explicit; even though
// execution starts locally, unrestricted scripts may use networks and billing.
TEST(ShellProvider, descriptor_declares_unrestricted_egress_and_is_not_idempotent) {
    ayni::providers::shell::Provider provider;
    ayni::providers::shell::Config config;
    config.working_directory = "/tmp";
    ASSERT_EQ(provider.configure(config), AYNI_OK);
    ayni_provider_v1 descriptor{};
    ayni_capability_desc_v1 authority{};
    ASSERT_EQ(provider.describe(descriptor), AYNI_OK);
    ASSERT_EQ(provider.describe_authority(authority), AYNI_OK);
    ASSERT_EQ(descriptor.capability_count, 1U);
    const auto &contract = descriptor.capabilities[0].contract;
    EXPECT_EQ(contract.idempotency, AYNI_CALL_NOT_IDEMPOTENT);
    EXPECT_EQ(contract.effects, AYNI_CALL_EFFECT_EGRESS | AYNI_CALL_EFFECT_BILLING);
    EXPECT_EQ(authority.field_count, 1U);
    EXPECT_EQ(authority.fields[0].usage, AYNI_AUTHORITY_EGRESS_DESTINATION);
    EXPECT_EQ(provider.pending(), 0U);
    EXPECT_EQ(provider.quiesce(), AYNI_OK);
    EXPECT_EQ(provider.close(), AYNI_OK);
}
} // namespace
