// ADR-0049/0050: the public Mock executor has explicit startup/close failures;
// missing local state never fabricates a permit, stop proof or borrowed output.
#include <ayni/providers/mock_execution.h>
#include <gtest/gtest.h>

namespace {
using namespace ayni::providers::mock_execution;
Config configuration() {
    Config result;
    result.now_us = [](void *) -> uint64_t { return 0; };
    result.limits.count = 1;
    result.limits.fields[0].field_id = {3, {'y', 'a', 'w'}};
    result.limits.fields[0].unit = {4, {'m', 'r', 'a', 'd'}};
    result.limits.fields[0].minimum = -100;
    result.limits.fields[0].maximum = 100;
    return result;
}

TEST(MockExecutor, unconfigured_operations_preserve_outputs_and_report_missing_lifecycle) {
    Executor executor;
    ayni_provider_v1 provider{};
    provider.version = 99;
    ayni_executor_v1 ports{};
    ports.version = 99;
    ayni_control_info_v1 state{};
    state.struct_size = sizeof(state);
    state.phase = 99;
    ayni_control_observation_v1 observation{};
    observation.interlocks_ok = 99;
    EXPECT_EQ(executor.describe_provider(provider), AYNI_INVALID_STATE);
    EXPECT_EQ(provider.version, 99U);
    EXPECT_EQ(executor.describe_executor(ports), AYNI_INVALID_STATE);
    EXPECT_EQ(ports.version, 99U);
    EXPECT_EQ(executor.info(state), AYNI_INVALID_STATE);
    EXPECT_EQ(state.phase, 99U);
    EXPECT_EQ(executor.observation(observation), AYNI_NOT_READY);
    EXPECT_EQ(observation.interlocks_ok, 99U);
    EXPECT_EQ(executor.observe(observation), AYNI_NOT_READY);
    EXPECT_EQ(executor.advance(), AYNI_INVALID_STATE);
    EXPECT_EQ(executor.set_stop_confirmation(StopConfirmation::manual), AYNI_NOT_READY);
    EXPECT_EQ(executor.confirm_stopped(), AYNI_INVALID_STATE);
    EXPECT_EQ(executor.answer(), AYNI_INVALID_STATE);
    EXPECT_EQ(executor.answer_renewals(), AYNI_INVALID_STATE);
    EXPECT_EQ(executor.replay_permit(), AYNI_NOT_READY);
    EXPECT_EQ(executor.replay_renewal(), AYNI_NOT_FOUND);
    EXPECT_EQ(executor.evict_challenges(), AYNI_NOT_READY);
    EXPECT_EQ(executor.command_without_permit(), AYNI_NOT_READY);
    EXPECT_EQ(executor.power_cycle(), AYNI_INVALID_STATE);
    EXPECT_EQ(executor.last_renewal_status(), AYNI_INVALID_STATE);
    EXPECT_EQ(executor.set_interlock(1), AYNI_INVALID_ARGUMENT);
    executor.set_link(Link::disconnected);
    executor.drop_requests();
    executor.abandon_kernel();
    EXPECT_EQ(executor.pending(), 0U);
    EXPECT_EQ(executor.held(AYNI_EXECUTOR_CHALLENGE), 0U);
    EXPECT_EQ(executor.samples(), 0U);
    EXPECT_EQ(executor.renewals(), 0U);
    EXPECT_EQ(executor.challenges(), 0U);
    EXPECT_EQ(executor.submissions(), 0U);
    EXPECT_EQ(executor.logical_call(), 0U);
    EXPECT_EQ(executor.close(), AYNI_OK);
    EXPECT_EQ(executor.close(), AYNI_OK);
}

TEST(MockExecutor, failed_configuration_can_be_retried_without_an_inert_partial_instance) {
    Executor executor;
    const auto changes = {
        +[](Config &c) { c.now_us = nullptr; },
        +[](Config &c) { c.executor_boot_epoch = 0; },
        +[](Config &c) { c.ticks_to_complete = 0; },
        +[](Config &c) { c.call_timeout_ms = 0; },
        +[](Config &c) { c.limits.count = 0; },
        +[](Config &c) { c.limits.count = AYNI_CONTROL_FIELD_COUNT_MAX + 1; },
    };
    for (const auto change : changes) {
        auto candidate = configuration();
        change(candidate);
        EXPECT_EQ(executor.configure(candidate), AYNI_INVALID_ARGUMENT);
        ayni_provider_v1 absent{};
        EXPECT_EQ(executor.describe_provider(absent), AYNI_INVALID_STATE);
    }
    ASSERT_EQ(executor.configure(configuration()), AYNI_OK);
    EXPECT_EQ(executor.configure(configuration()), AYNI_INVALID_STATE);
    EXPECT_EQ(executor.set_stop_confirmation(static_cast<StopConfirmation>(UINT32_MAX)),
              AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(executor.set_stop_confirmation(StopConfirmation::manual), AYNI_OK);
    EXPECT_EQ(executor.set_stop_confirmation(StopConfirmation::automatic), AYNI_OK);
    EXPECT_EQ(executor.set_interlock(2), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(executor.set_interlock(1), AYNI_OK);
    ayni_control_info_v1 state{};
    EXPECT_EQ(executor.info(state), AYNI_INVALID_ARGUMENT);
    state.struct_size = sizeof(state);
    ASSERT_EQ(executor.info(state), AYNI_OK);
    EXPECT_EQ(state.phase, AYNI_CONTROL_IDLE);
    EXPECT_EQ(state.permit_id, 0U);
    EXPECT_EQ(executor.confirm_stopped(), AYNI_NOT_READY);
    EXPECT_EQ(executor.close(), AYNI_OK);
    EXPECT_EQ(executor.configure(configuration()), AYNI_OK);
    EXPECT_EQ(executor.pending(), 0U);
    EXPECT_EQ(executor.submissions(), 0U);
    EXPECT_EQ(executor.close(), AYNI_OK);
}

TEST(MockExecutor,
     missing_request_inputs_or_completion_never_retain_a_provider_or_executor_borrow) {
    Executor executor;
    ASSERT_EQ(executor.configure(configuration()), AYNI_OK);
    ayni_provider_v1 provider{};
    ayni_executor_v1 ports{};
    ASSERT_EQ(executor.describe_provider(provider), AYNI_OK);
    ASSERT_EQ(executor.describe_executor(ports), AYNI_OK);
    ayni_provider_request_v1 request{};
    request.struct_size = sizeof(request);
    EXPECT_EQ(provider.submit(provider.ctx, nullptr, nullptr), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    EXPECT_EQ(provider.submit(provider.ctx, &request, nullptr), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    EXPECT_EQ(provider.cancel(provider.ctx, {1, 1}), AYNI_SUBMIT_REJECTED_NOT_RETAINED);
    ayni_permit_epochs_v1 epochs{1, 1, 1, 1, 1, 0};
    ayni_permit_renewal_v1 renewal{};
    renewal.struct_size = sizeof(renewal);
    EXPECT_EQ(ports.challenge(ports.ctx, {1, 1}, nullptr, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ports.challenge(ports.ctx, {1, 1}, &epochs, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ports.renew(ports.ctx, {1, 1}, nullptr, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ports.renew(ports.ctx, {1, 1}, &renewal, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ports.stop(ports.ctx, {1, 1}, 1, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(ports.status(ports.ctx, {1, 1}, 1, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(executor.pending(), 0U);
    EXPECT_EQ(executor.held(AYNI_EXECUTOR_CHALLENGE), 0U);
    EXPECT_EQ(executor.held(AYNI_EXECUTOR_RENEW), 0U);
    EXPECT_EQ(executor.challenges(), 0U);
    EXPECT_EQ(executor.submissions(), 0U);
    EXPECT_EQ(executor.samples(), 0U);
    EXPECT_EQ(executor.close(), AYNI_OK);
}
} // namespace
