// C-PERMIT-003/004/006/007/008/010/012 and C-STOP-006, ADR-0049.
// Normal lifecycle and public input boundaries; no fault/recovery campaign.
#include <ayni/control.h>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {
ayni_catalog_name_v1 name(const char *value) {
    ayni_catalog_name_v1 out{};
    out.size = static_cast<uint32_t>(std::strlen(value));
    std::memcpy(out.data, value, out.size);
    return out;
}
ayni_control_config_v1 config() {
    ayni_control_config_v1 out{};
    out.struct_size = sizeof(out);
    out.abi_major = AYNI_ABI_MAJOR;
    out.abi_minor = AYNI_ABI_MINOR;
    out.control_mode = 1;
    out.control_domain_id = 7;
    out.epochs = {1, 2, 3, 4, 5, 0};
    out.procedure_max_us = 2000;
    out.session_lease_max_us = 1000;
    out.command_age_max_us = 100;
    out.limits.count = 2;
    out.limits.fields[0] = {name("yaw"), name("mrad-s"), -100, 100};
    out.limits.fields[1] = {name("speed"), name("mm-s"), -200, 200};
    return out;
}
class ControlGate : public ::testing::Test {
  protected:
    void SetUp() override {
        size_t bytes = 0;
        size_t alignment = 0;
        ASSERT_EQ(ayni_control_memory_requirements(&bytes, &alignment), AYNI_OK);
        ASSERT_LE(alignment, alignof(uint64_t));
        memory.resize((bytes + sizeof(uint64_t) - 1) / sizeof(uint64_t));
        ASSERT_EQ(ayni_control_create(&settings, 0, memory.data(), bytes, &gate), AYNI_OK);
        live = {settings.epochs, 1, 0};
    }
    void TearDown() override {
        const auto state = info();
        if (state.phase == AYNI_CONTROL_ACTIVE) {
            EXPECT_EQ(ayni_control_stop(gate, state.permit_id, 1000000), AYNI_OK);
        }
        if (info().phase == AYNI_CONTROL_STOPPING) {
            EXPECT_EQ(ayni_control_confirm_stopped(gate, state.permit_id, 1000000), AYNI_OK);
        }
        EXPECT_EQ(ayni_control_destroy(gate), AYNI_OK);
    }
    ayni_control_info_v1 info() {
        ayni_control_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_control_get_info(gate, &out), AYNI_OK);
        return out;
    }
    uint64_t nonce(uint64_t at = 0) {
        ayni_executor_nonce_v1 out{};
        EXPECT_EQ(ayni_control_nonce(gate, at, &out), AYNI_OK);
        EXPECT_EQ(out.control_domain_id, settings.control_domain_id);
        return out.nonce;
    }
    ayni_execution_permit_v1 permit(uint32_t form = AYNI_FORM_SESSION, uint64_t challenge = 0) {
        ayni_execution_permit_v1 out{};
        out.struct_size = sizeof(out);
        out.form = form;
        out.permit_id = 10;
        out.operation_id = 20;
        out.control_domain_id = settings.control_domain_id;
        out.epochs = settings.epochs;
        out.echoed_nonce = challenge == 0 ? nonce() : challenge;
        out.control_mode = settings.control_mode;
        out.lease_us = 1000;
        out.max_command_age_us = 100;
        out.envelope = settings.limits;
        out.envelope.fields[0].minimum = -50;
        out.envelope.fields[0].maximum = 50;
        return out;
    }
    ayni_control_sample_v1 sample(uint64_t at = 0) {
        ayni_control_sample_v1 out{};
        out.struct_size = sizeof(out);
        out.count = 2;
        out.permit_id = 10;
        out.sequence = 1;
        out.sampled_at_us = at;
        out.fields[0] = {name("speed"), name("mm-s"), 30};
        out.fields[1] = {name("yaw"), name("mrad-s"), -50};
        return out;
    }
    ayni_permit_renewal_v1 renewal(uint64_t challenge) {
        return {sizeof(ayni_permit_renewal_v1), 0, 10, challenge, settings.epochs};
    }
    ayni_control_config_v1 settings = config();
    ayni_control_observation_v1 live{};
    std::vector<uint64_t> memory;
    ayni_control_gate_t *gate = nullptr;
};

TEST_F(ControlGate, startup_requires_a_permit_and_first_sample_before_output) {
    EXPECT_EQ(info().phase, AYNI_CONTROL_IDLE);
    EXPECT_EQ(ayni_control_check(gate, 0, &live), AYNI_INVALID_STATE);
    auto command = sample();
    EXPECT_EQ(ayni_control_sample(gate, &command, 0, &live), AYNI_INVALID_STATE);
    auto allowed = permit();
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    EXPECT_EQ(ayni_control_check(gate, 0, &live), AYNI_NOT_READY);
    EXPECT_EQ(ayni_control_sample(gate, &command, 0, &live), AYNI_OK);
    EXPECT_EQ(ayni_control_check(gate, 100, &live), AYNI_OK);
    EXPECT_EQ(info().last_sample_sequence, 1U);
}

TEST_F(ControlGate, lease_anchors_at_nonce_and_renewal_never_uses_arrival_time) {
    auto allowed = permit(AYNI_FORM_SESSION, nonce(100));
    ASSERT_EQ(ayni_control_start(gate, &allowed, 300, &live), AYNI_OK);
    EXPECT_EQ(info().deadline_us, 1100U);
    auto renew = renewal(nonce(500));
    ASSERT_EQ(ayni_control_renew(gate, &renew, 700, &live), AYNI_OK);
    EXPECT_EQ(info().deadline_us, 1500U);
    renew.echoed_nonce = allowed.echoed_nonce;
    EXPECT_EQ(ayni_control_renew(gate, &renew, 800, &live), AYNI_OK);
    EXPECT_EQ(info().deadline_us, 1500U);
}

TEST_F(ControlGate, procedure_is_nonrenewable_and_expiry_is_permanently_terminal) {
    auto allowed = permit(AYNI_FORM_PROCEDURE);
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    auto renew = renewal(nonce(500));
    EXPECT_EQ(ayni_control_renew(gate, &renew, 500, &live), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_control_check(gate, 1000, &live), AYNI_EXPIRED);
    EXPECT_EQ(info().phase, AYNI_CONTROL_STOPPING);
    EXPECT_EQ(info().stop_cause, AYNI_CONTROL_STOP_EXPIRED);
    EXPECT_EQ(ayni_control_renew(gate, &renew, 1000, &live), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_control_destroy(gate), AYNI_BUSY);
    EXPECT_EQ(ayni_control_confirm_stopped(gate, 10, 1000), AYNI_OK);
    EXPECT_EQ(ayni_control_start(gate, &allowed, 1000, &live), AYNI_STALE);
    allowed.permit_id = 11;
    allowed.operation_id = 21;
    allowed.echoed_nonce = nonce(1000);
    EXPECT_EQ(ayni_control_start(gate, &allowed, 1000, &live), AYNI_OK);
}

TEST_F(ControlGate, cancellation_and_stop_measurement_are_separate_facts) {
    auto allowed = permit();
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    EXPECT_EQ(ayni_control_confirm_stopped(gate, 10, 0), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_control_stop(gate, 10, 50), AYNI_OK);
    EXPECT_EQ(info().phase, AYNI_CONTROL_STOPPING);
    EXPECT_EQ(ayni_control_stop(gate, 11, 60), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_control_confirm_stopped(gate, 11, 60), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_control_start(gate, &allowed, 60, &live), AYNI_BUSY);
    EXPECT_EQ(ayni_control_confirm_stopped(gate, 10, 59), AYNI_STALE);
    EXPECT_EQ(ayni_control_confirm_stopped(gate, 10, 70), AYNI_OK);
    EXPECT_EQ(ayni_control_confirm_stopped(gate, 10, 80), AYNI_OK);
    EXPECT_EQ(info().stop_confirmed_at_us, 70U);
    EXPECT_EQ(info().phase, AYNI_CONTROL_STOPPED);
    EXPECT_EQ(info().stop_cause, AYNI_CONTROL_STOP_REQUESTED);
}

TEST_F(ControlGate, control_cycles_expire_the_last_sample_without_waiting_for_lease_end) {
    auto allowed = permit();
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    auto command = sample(100);
    ASSERT_EQ(ayni_control_sample(gate, &command, 100, &live), AYNI_OK);
    EXPECT_EQ(ayni_control_check(gate, 201, &live), AYNI_EXPIRED);
    EXPECT_EQ(info().stop_cause, AYNI_CONTROL_STOP_SAMPLE);
    command.sequence = 2;
    command.sampled_at_us = 201;
    EXPECT_EQ(ayni_control_sample(gate, &command, 201, &live), AYNI_INVALID_STATE);
}

TEST_F(ControlGate, samples_require_exact_fields_units_bounds_and_increasing_sequence) {
    auto allowed = permit();
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    auto command = sample();
    ASSERT_EQ(ayni_control_sample(gate, &command, 0, &live), AYNI_OK);
    EXPECT_EQ(ayni_control_sample(gate, &command, 0, &live), AYNI_DENIED);
    EXPECT_EQ(info().phase, AYNI_CONTROL_STOPPING);
}

TEST_F(ControlGate, an_out_of_bounds_sample_stops_instead_of_clamping) {
    auto allowed = permit();
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    auto command = sample();
    command.fields[1].value = -51;
    EXPECT_EQ(ayni_control_sample(gate, &command, 0, &live), AYNI_DENIED);
    EXPECT_EQ(info().last_sample_sequence, 0U);
    EXPECT_EQ(info().stop_cause, AYNI_CONTROL_STOP_SAMPLE);
}

TEST_F(ControlGate, missing_duplicate_wrong_unit_and_stale_samples_stop_the_permit) {
    for (uint32_t kind = 0; kind < 5; ++kind) {
        const uint64_t now = 2000 * kind;
        auto allowed = permit(AYNI_FORM_SESSION, nonce(now));
        allowed.permit_id += kind;
        ASSERT_EQ(ayni_control_start(gate, &allowed, now, &live), AYNI_OK);
        auto command = sample(now);
        command.permit_id = allowed.permit_id;
        switch (kind) {
        case 0:
            command.count = 1;
            break;
        case 1:
            command.fields[0] = command.fields[1];
            break;
        case 2:
            command.fields[0].unit = name("other");
            break;
        case 3:
            command.sampled_at_us = now + 1;
            break;
        case 4:
            command.sampled_at_us = now - 101;
            break;
        }
        EXPECT_EQ(ayni_control_sample(gate, &command, now, &live), AYNI_DENIED);
        EXPECT_EQ(info().phase, AYNI_CONTROL_STOPPING);
        ASSERT_EQ(ayni_control_confirm_stopped(gate, allowed.permit_id, now), AYNI_OK);
    }
}

TEST_F(ControlGate, incomplete_or_enlarged_envelopes_are_rejected_before_start) {
    auto allowed = permit();
    allowed.envelope.count = 1;
    EXPECT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_DENIED);
    allowed.envelope = settings.limits;
    allowed.envelope.fields[0].maximum = 101;
    EXPECT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_DENIED);
    allowed.envelope = settings.limits;
    allowed.envelope.fields[0].unit = name("deg-s");
    EXPECT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_DENIED);
    allowed.envelope.fields[0] = allowed.envelope.fields[1];
    EXPECT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(info().phase, AYNI_CONTROL_IDLE);
}

TEST_F(ControlGate, unknown_evicted_and_expired_challenges_do_not_start_execution) {
    const auto old = nonce();
    auto allowed = permit(AYNI_FORM_SESSION, old);
    for (uint32_t at = 0; at < AYNI_CONTROL_NONCE_COUNT; ++at) {
        nonce(1);
    }
    EXPECT_EQ(ayni_control_start(gate, &allowed, 1, &live), AYNI_STALE);
    allowed.echoed_nonce = 9999;
    EXPECT_EQ(ayni_control_start(gate, &allowed, 1, &live), AYNI_STALE);
    allowed.echoed_nonce = nonce(1);
    EXPECT_EQ(ayni_control_start(gate, &allowed, 1001, &live), AYNI_EXPIRED);
    EXPECT_EQ(info().phase, AYNI_CONTROL_IDLE);
}

TEST_F(ControlGate, renewed_sessions_cannot_resume_after_the_exact_deadline) {
    auto allowed = permit();
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    auto renew = renewal(nonce(900));
    EXPECT_EQ(ayni_control_renew(gate, &renew, 1000, &live), AYNI_EXPIRED);
    EXPECT_EQ(info().deadline_us, 1000U);
    EXPECT_EQ(ayni_control_renew(gate, &renew, 1001, &live), AYNI_INVALID_STATE);
}

TEST_F(ControlGate, epoch_and_interlock_changes_do_not_allow_output_or_renewal) {
    auto allowed = permit();
    auto mismatched = allowed;
    ++mismatched.epochs.executor_boot_epoch;
    EXPECT_EQ(ayni_control_start(gate, &mismatched, 0, &live), AYNI_DENIED);
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    ++live.epochs.calibration_version;
    EXPECT_EQ(ayni_control_check(gate, 1, &live), AYNI_DENIED);
    EXPECT_EQ(info().stop_cause, AYNI_CONTROL_STOP_EPOCH);
    live.epochs = settings.epochs;
    EXPECT_EQ(ayni_control_check(gate, 2, &live), AYNI_INVALID_STATE);
}

TEST_F(ControlGate, a_local_interlock_stops_without_queue_or_provider_acknowledgement) {
    auto allowed = permit();
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    live.interlocks_ok = 0;
    EXPECT_EQ(ayni_control_check(gate, 1, &live), AYNI_DENIED);
    EXPECT_EQ(info().stop_cause, AYNI_CONTROL_STOP_INTERLOCK);
    live.interlocks_ok = 1;
    EXPECT_EQ(ayni_control_check(gate, 2, &live), AYNI_INVALID_STATE);
}

TEST_F(ControlGate, all_eight_fields_are_supported_and_permit_inputs_are_copied) {
    ASSERT_EQ(ayni_control_destroy(gate), AYNI_OK);
    settings.limits = {};
    settings.limits.count = AYNI_CONTROL_FIELD_COUNT_MAX;
    constexpr char field_names[] = "abcdefgh";
    for (uint32_t at = 0; at < AYNI_CONTROL_FIELD_COUNT_MAX; ++at) {
        const char key[] = {field_names[at], '\0'};
        settings.limits.fields[at] = {name(key), name("u"), -1, 1};
    }
    ++settings.epochs.executor_boot_epoch; // Every gate lifetime uses a fresh generation.
    live.epochs = settings.epochs;
    ASSERT_EQ(
        ayni_control_create(&settings, 0, memory.data(), memory.size() * sizeof(uint64_t), &gate),
        AYNI_OK);
    auto allowed = permit();
    allowed.envelope = settings.limits;
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    auto command = sample();
    command.count = AYNI_CONTROL_FIELD_COUNT_MAX;
    for (uint32_t at = 0; at < command.count; ++at) {
        const auto &field = settings.limits.fields[at];
        command.fields[at] = {field.field_id, field.unit, 1};
    }
    allowed.envelope.fields[0].maximum = 0;
    settings.limits.fields[0].maximum = 0;
    EXPECT_EQ(ayni_control_sample(gate, &command, 0, &live), AYNI_OK);
}

TEST_F(ControlGate, short_renewal_header_is_rejected_before_reading_identity) {
    auto allowed = permit();
    ASSERT_EQ(ayni_control_start(gate, &allowed, 0, &live), AYNI_OK);
    alignas(ayni_permit_renewal_v1) const uint32_t short_header = sizeof(uint32_t);
    const auto *short_renewal = reinterpret_cast<const ayni_permit_renewal_v1 *>(&short_header);
    EXPECT_EQ(ayni_control_renew(gate, short_renewal, 0, &live), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(info().phase, AYNI_CONTROL_ACTIVE);
    EXPECT_EQ(info().deadline_us, 1000U);
}

TEST_F(ControlGate, caller_storage_and_snapshot_boundaries_preserve_outputs) {
    ayni_control_gate_t *unchanged = gate;
    EXPECT_EQ(ayni_control_create(&settings, 0, memory.data(), 1, &unchanged),
              AYNI_BUFFER_TOO_SMALL);
    EXPECT_EQ(unchanged, gate);
    auto invalid = settings;
    invalid.struct_size += 1;
    EXPECT_EQ(ayni_control_create(&invalid, 0, memory.data(), memory.size() * sizeof(uint64_t),
                                  &unchanged),
              AYNI_UNSUPPORTED_VERSION);
    invalid = settings;
    invalid.abi_minor += 1;
    EXPECT_EQ(ayni_control_create(&invalid, 0, memory.data(), memory.size() * sizeof(uint64_t),
                                  &unchanged),
              AYNI_UNSUPPORTED_VERSION);
    invalid = settings;
    invalid.limits.count = AYNI_CONTROL_FIELD_COUNT_MAX + 1;
    EXPECT_EQ(ayni_control_create(&invalid, 0, memory.data(), memory.size() * sizeof(uint64_t),
                                  &unchanged),
              AYNI_INVALID_ARGUMENT);
    auto out = info();
    out.struct_size += 1;
    out.permit_id = 999;
    EXPECT_EQ(ayni_control_get_info(gate, &out), AYNI_UNSUPPORTED_VERSION);
    EXPECT_EQ(out.permit_id, 999U);
    EXPECT_EQ(ayni_control_nonce(nullptr, 0, nullptr), AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(info().phase, AYNI_CONTROL_IDLE);
}
} // namespace
