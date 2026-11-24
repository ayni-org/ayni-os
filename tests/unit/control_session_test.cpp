// ADR-0112: portable zero-heap ControlSession streaming multi-segment execution over
// ayni_control_gate (ABI 0.59). Verifies seamless time- and hardware-timed transitions,
// ring compaction, live target replacement, short-lease renewal, and fail-closed boundaries.

#include <ayni/control_session.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

ayni_catalog_name_v1 catalog_name(const char *value) {
    ayni_catalog_name_v1 out{};
    out.size = static_cast<uint32_t>(std::strlen(value));
    std::memcpy(out.data, value, out.size);
    return out;
}

ayni_control_config_v1 make_config() {
    ayni_control_config_v1 out{};
    out.struct_size = sizeof(out);
    out.abi_major = AYNI_ABI_MAJOR;
    out.abi_minor = AYNI_ABI_MINOR;
    out.control_mode = 1;
    out.control_domain_id = 11;
    out.epochs = {1, 2, 3, 4, 5, 0};
    out.procedure_max_us = 2000;
    out.session_lease_max_us = 1000;
    out.command_age_max_us = 200;
    out.limits.count = 2;
    out.limits.fields[0] = {catalog_name("m1"), catalog_name("percent"), -90, 90};
    out.limits.fields[1] = {catalog_name("m2"), catalog_name("percent"), -90, 90};
    return out;
}

ayni_control_segment_v1 make_segment(int64_t m1, int64_t m2, uint64_t duration_us) {
    ayni_control_segment_v1 seg{};
    seg.struct_size = sizeof(seg);
    seg.field_count = 2;
    seg.duration_us = duration_us;
    seg.fields[0] = {catalog_name("m1"), catalog_name("percent"), m1};
    seg.fields[1] = {catalog_name("m2"), catalog_name("percent"), m2};
    return seg;
}

class ControlSessionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        size_t gate_bytes = 0;
        size_t gate_align = 0;
        ASSERT_EQ(ayni_control_memory_requirements(&gate_bytes, &gate_align), AYNI_OK);
        ASSERT_LE(gate_align, alignof(uint64_t));
        gate_mem.resize((gate_bytes + sizeof(uint64_t) - 1U) / sizeof(uint64_t));
        ASSERT_EQ(ayni_control_create(&settings, 0, gate_mem.data(), gate_bytes, &gate), AYNI_OK);

        size_t sess_bytes = 0;
        size_t sess_align = 0;
        ASSERT_EQ(ayni_control_session_memory_requirements(&sess_bytes, &sess_align), AYNI_OK);
        ASSERT_LE(sess_align, alignof(uint64_t));
        session_mem.resize((sess_bytes + sizeof(uint64_t) - 1U) / sizeof(uint64_t));
        ASSERT_EQ(ayni_control_session_create(session_mem.data(), sess_bytes, &session), AYNI_OK);

        live = {settings.epochs, 1U, 0U};
    }

    void TearDown() override {
        const auto sinfo = session_info();
        if (sinfo.state == AYNI_CONTROL_SESSION_ACTIVE) {
            EXPECT_EQ(ayni_control_session_soft_stop(session, 1000000), AYNI_OK);
        }
        const auto after_stop = session_info();
        if (after_stop.state == AYNI_CONTROL_SESSION_STOPPING ||
            after_stop.state == AYNI_CONTROL_SESSION_COMPLETED) {
            EXPECT_EQ(ayni_control_session_confirm_stopped(session, 1000000), AYNI_OK);
        }
        EXPECT_EQ(ayni_control_session_destroy(session), AYNI_OK);
        EXPECT_EQ(ayni_control_destroy(gate), AYNI_OK);
    }

    uint64_t next_nonce(uint64_t at_us) {
        ayni_executor_nonce_v1 out{};
        EXPECT_EQ(ayni_control_nonce(gate, at_us, &out), AYNI_OK);
        return out.nonce;
    }

    ayni_execution_permit_v1 make_permit(uint32_t form, uint64_t permit_id, uint64_t at_us,
                                         uint32_t lease_us = 1000, int64_t lo = -60,
                                         int64_t hi = 60) {
        ayni_execution_permit_v1 out{};
        out.struct_size = sizeof(out);
        out.form = form;
        out.permit_id = permit_id;
        out.operation_id = permit_id * 10U;
        out.control_domain_id = settings.control_domain_id;
        out.epochs = settings.epochs;
        out.echoed_nonce = next_nonce(at_us);
        out.control_mode = settings.control_mode;
        out.lease_us = lease_us;
        out.max_command_age_us = 200;
        out.envelope = settings.limits;
        out.envelope.fields[0].minimum = lo;
        out.envelope.fields[0].maximum = hi;
        out.envelope.fields[1].minimum = lo;
        out.envelope.fields[1].maximum = hi;
        return out;
    }

    ayni_control_session_info_v1 session_info() {
        ayni_control_session_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_control_session_get_info(session, &out), AYNI_OK);
        return out;
    }

    ayni_control_info_v1 gate_info() {
        ayni_control_info_v1 out{};
        out.struct_size = sizeof(out);
        EXPECT_EQ(ayni_control_get_info(gate, &out), AYNI_OK);
        return out;
    }

    ayni_control_config_v1 settings = make_config();
    ayni_control_observation_v1 live{};
    std::vector<uint64_t> gate_mem;
    std::vector<uint64_t> session_mem;
    ayni_control_gate_t *gate = nullptr;
    ayni_control_session_t *session = nullptr;
};

// ADR-0112: Multi-segment time-bounded trajectory transitions atomically inside one SESSION
// without stopping the PermitGate, then confirms clean stop and allows reopening a fresh permit.
TEST_F(ControlSessionTest, MultiSegmentSeamlessTickAndStopConfirmation) {
    const auto permit = make_permit(AYNI_FORM_SESSION, 10, 0, 1000, -50, 50);
    const std::array<ayni_control_segment_v1, 3> segments{
        make_segment(40, 40, 200),
        make_segment(0, 0, 300),
        make_segment(-30, 30, 200),
    };
    ASSERT_EQ(ayni_control_session_open(session, gate, &permit, segments.data(),
                                        static_cast<uint32_t>(segments.size()), 0, &live),
              AYNI_OK);

    ayni_control_session_step_v1 step{};
    step.struct_size = sizeof(step);

    ASSERT_EQ(ayni_control_session_tick(session, 0, &live, &step), AYNI_OK);
    EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_ACTIVE);
    EXPECT_EQ(step.segment_index, 0U);
    EXPECT_EQ(step.segment_count, 3U);
    EXPECT_EQ(step.transitioned, 1U);
    EXPECT_EQ(step.remaining_us, 200U);
    EXPECT_EQ(step.active.fields[0].value, 40);

    ASSERT_EQ(ayni_control_session_tick(session, 100, &live, &step), AYNI_OK);
    EXPECT_EQ(step.segment_index, 0U);
    EXPECT_EQ(step.transitioned, 0U);
    EXPECT_EQ(step.remaining_us, 100U);

    // At t=200 segment 0 expires -> seamless transition to segment 1 with zero gate stop.
    ASSERT_EQ(ayni_control_session_tick(session, 200, &live, &step), AYNI_OK);
    EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_ACTIVE);
    EXPECT_EQ(step.segment_index, 1U);
    EXPECT_EQ(step.transitioned, 1U);
    EXPECT_EQ(step.remaining_us, 300U);
    EXPECT_EQ(step.active.fields[0].value, 0);
    EXPECT_EQ(gate_info().phase, AYNI_CONTROL_ACTIVE);

    ASSERT_EQ(ayni_control_session_tick(session, 350, &live, &step), AYNI_OK);
    EXPECT_EQ(step.segment_index, 1U);
    EXPECT_EQ(step.transitioned, 0U);
    EXPECT_EQ(step.remaining_us, 150U);

    // At t=500 segment 1 expires -> seamless transition to segment 2.
    ASSERT_EQ(ayni_control_session_tick(session, 500, &live, &step), AYNI_OK);
    EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_ACTIVE);
    EXPECT_EQ(step.segment_index, 2U);
    EXPECT_EQ(step.transitioned, 1U);
    EXPECT_EQ(step.remaining_us, 200U);
    EXPECT_EQ(step.active.fields[0].value, -30);
    EXPECT_EQ(step.active.fields[1].value, 30);

    // At t=700 final segment ends -> COMPLETED (caller disables hardware & confirms stopped).
    ASSERT_EQ(ayni_control_session_tick(session, 700, &live, &step), AYNI_OK);
    EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_COMPLETED);
    EXPECT_EQ(step.transitioned, 0U);

    const auto info = session_info();
    EXPECT_EQ(info.state, AYNI_CONTROL_SESSION_COMPLETED);
    EXPECT_EQ(info.permit_id, 10U);
    EXPECT_EQ(info.transitions, 2U);
    EXPECT_EQ(info.last_sample_sequence, 5U);

    // Cannot destroy before confirming local hardware stop.
    EXPECT_EQ(ayni_control_session_destroy(session), AYNI_BUSY);
    ASSERT_EQ(ayni_control_session_confirm_stopped(session, 710), AYNI_OK);
    EXPECT_EQ(session_info().state, AYNI_CONTROL_SESSION_STOPPED);
    EXPECT_EQ(gate_info().phase, AYNI_CONTROL_STOPPED);

    // Reopen cleanly with a second permit on the same session instance.
    const auto second_permit = make_permit(AYNI_FORM_SESSION, 11, 720, 1000, -50, 50);
    const auto one = make_segment(25, -25, 100);
    ASSERT_EQ(ayni_control_session_open(session, gate, &second_permit, &one, 1, 720, &live),
              AYNI_OK);
    EXPECT_EQ(session_info().permit_id, 11U);
}

// ADR-0112: Hardware-timed segments (duration_us == 0) never prematurely switch on tick(),
// advance atomically via advance_segment(), and compact consumed prefix so streaming sessions
// can execute more than AYNI_CONTROL_SESSION_SEGMENTS_MAX total segments without reallocation.
TEST_F(ControlSessionTest, HardwareTimedAdvanceAndRingCompactionAcrossMoreThanMaxSegments) {
    const auto permit = make_permit(AYNI_FORM_SESSION, 20, 0, 1000, -60, 60);
    std::array<ayni_control_segment_v1, AYNI_CONTROL_SESSION_SEGMENTS_MAX> initial{};
    for (uint32_t at = 0; at < AYNI_CONTROL_SESSION_SEGMENTS_MAX; ++at) {
        initial[at] = make_segment(static_cast<int64_t>(at * 5), -static_cast<int64_t>(at * 5), 0);
    }
    ASSERT_EQ(ayni_control_session_open(session, gate, &permit, initial.data(),
                                        AYNI_CONTROL_SESSION_SEGMENTS_MAX, 0, &live),
              AYNI_OK);

    // Full with segment_index == 0 rejects extra pushes cleanly.
    const auto extra = make_segment(50, 50, 0);
    EXPECT_EQ(ayni_control_session_push_segment(session, &extra), AYNI_CAPACITY_EXCEEDED);

    ayni_control_session_step_v1 step{};
    step.struct_size = sizeof(step);
    uint64_t now = 0;
    ASSERT_EQ(ayni_control_session_tick(session, now, &live, &step), AYNI_OK);
    EXPECT_EQ(step.segment_index, 0U);
    EXPECT_EQ(step.remaining_us, 0U);

    // Tick keeps holding segment 0 even as time advances (hardware controls switch timing).
    now += 50;
    ASSERT_EQ(ayni_control_session_tick(session, now, &live, &step), AYNI_OK);
    EXPECT_EQ(step.segment_index, 0U);
    EXPECT_EQ(step.transitioned, 0U);

    // Advance 4 segments via hardware completion, then push 4 more (compacting prefix).
    for (uint32_t k = 1; k <= 4; ++k) {
        now += 50;
        ASSERT_EQ(ayni_control_session_advance_segment(session, now, &live, &step), AYNI_OK);
        EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_ACTIVE);
        EXPECT_EQ(step.segment_index, k);
        EXPECT_EQ(step.transitioned, 1U);
        EXPECT_EQ(step.active.fields[0].value, static_cast<int64_t>(k * 5));
    }
    for (uint32_t k = 0; k < 4; ++k) {
        const auto tail = make_segment(static_cast<int64_t>(40 + k * 5), 10, 0);
        ASSERT_EQ(ayni_control_session_push_segment(session, &tail), AYNI_OK);
    }

    // Drain all remaining 7 segments (indices 1..7 after compaction) -> 11 total transitions.
    for (uint32_t k = 0; k < 7; ++k) {
        now += 50;
        ASSERT_EQ(ayni_control_session_advance_segment(session, now, &live, &step), AYNI_OK);
        EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_ACTIVE);
        EXPECT_EQ(step.transitioned, 1U);
    }
    now += 50;
    ASSERT_EQ(ayni_control_session_advance_segment(session, now, &live, &step), AYNI_OK);
    EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_COMPLETED);
    EXPECT_EQ(session_info().transitions, 11U);
}

// ADR-0112: Closed-loop / joystick replace_target() updates setpoint atomically within envelope
// and rejects out-of-envelope values without disturbing the active trajectory.
TEST_F(ControlSessionTest, ReplaceTargetInEnvelopeAndRejectOutOfEnvelope) {
    const auto permit = make_permit(AYNI_FORM_SESSION, 30, 0, 1000, -40, 40);
    const std::array<ayni_control_segment_v1, 2> segments{
        make_segment(20, 20, 400),
        make_segment(30, 30, 400),
    };
    ASSERT_EQ(ayni_control_session_open(session, gate, &permit, segments.data(), 2, 0, &live),
              AYNI_OK);

    ayni_control_session_step_v1 step{};
    step.struct_size = sizeof(step);
    ASSERT_EQ(ayni_control_session_tick(session, 0, &live, &step), AYNI_OK);
    EXPECT_EQ(step.active.fields[0].value, 20);

    // Out-of-envelope target is denied and preserves queued segments.
    const auto too_fast = make_segment(45, 20, 200);
    EXPECT_EQ(ayni_control_session_replace_target(session, &too_fast, 50), AYNI_DENIED);
    EXPECT_EQ(session_info().segment_count, 2U);

    // Valid target replaces trajectory and emits transitioned=1 on next tick.
    const auto valid = make_segment(-35, 15, 0);
    ASSERT_EQ(ayni_control_session_replace_target(session, &valid, 50), AYNI_OK);
    EXPECT_EQ(session_info().segment_count, 1U);
    ASSERT_EQ(ayni_control_session_tick(session, 60, &live, &step), AYNI_OK);
    EXPECT_EQ(step.transitioned, 1U);
    EXPECT_EQ(step.active.fields[0].value, -35);
    EXPECT_EQ(step.active.fields[1].value, 15);
}

// ADR-0112: Short-lease renewal extends an active ControlSession past its initial lease,
// while interlock drop or unrenewed lease expiry transitions fail-closed to STOPPING.
TEST_F(ControlSessionTest, ShortLeaseRenewalAndFailClosedOnExpiryOrInterlockDrop) {
    const auto permit = make_permit(AYNI_FORM_SESSION, 40, 0, 500, -50, 50);
    const auto seg = make_segment(25, 25, 0);
    ASSERT_EQ(ayni_control_session_open(session, gate, &permit, &seg, 1, 0, &live), AYNI_OK);

    ayni_control_session_step_v1 step{};
    step.struct_size = sizeof(step);
    ASSERT_EQ(ayni_control_session_tick(session, 0, &live, &step), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 150, &live, &step), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 300, &live, &step), AYNI_OK);

    // Renew at t=350 (anchored at nonce issue t=350 -> new deadline 850).
    const ayni_permit_renewal_v1 renewal{sizeof(renewal), 0, permit.permit_id, next_nonce(350),
                                         settings.epochs};
    ASSERT_EQ(ayni_control_session_renew(session, &renewal, 350, &live), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 450, &live, &step), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 600, &live, &step), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 750, &live, &step), AYNI_OK);
    EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_ACTIVE);

    // Interlock drop at t=800 stops immediately.
    auto unsafe = live;
    unsafe.interlocks_ok = 0;
    EXPECT_EQ(ayni_control_session_tick(session, 800, &unsafe, &step), AYNI_DENIED);
    EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_STOPPING);
    EXPECT_EQ(gate_info().phase, AYNI_CONTROL_STOPPING);
    ASSERT_EQ(ayni_control_session_confirm_stopped(session, 810), AYNI_OK);

    // Second permit: verify unrenewed lease expiry also enters STOPPING fail-closed.
    const auto second = make_permit(AYNI_FORM_SESSION, 41, 900, 500, -50, 50);
    ASSERT_EQ(ayni_control_session_open(session, gate, &second, &seg, 1, 900, &live), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 900, &live, &step), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 1050, &live, &step), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 1200, &live, &step), AYNI_OK);
    ASSERT_EQ(ayni_control_session_tick(session, 1350, &live, &step), AYNI_OK);
    EXPECT_EQ(ayni_control_session_tick(session, 1450, &live, &step), AYNI_EXPIRED);
    EXPECT_EQ(step.state, AYNI_CONTROL_SESSION_STOPPING);
    EXPECT_EQ(ayni_control_session_destroy(session), AYNI_BUSY);
}

// ADR-0112: open() validates every segment BEFORE arming PermitGate, and rejects multi-segment
// trajectories on AYNI_FORM_PROCEDURE.
TEST_F(ControlSessionTest, OpenRejectsInvalidSegmentsWithoutArmingGateAndRejectsMultiSegmentProc) {
    const auto session_permit = make_permit(AYNI_FORM_SESSION, 50, 0, 1000, -30, 30);
    const std::array<ayni_control_segment_v1, 2> bad_segments{
        make_segment(20, 20, 100), make_segment(35, 0, 100), // Exceeds envelope [-30, 30].
    };
    EXPECT_EQ(
        ayni_control_session_open(session, gate, &session_permit, bad_segments.data(), 2, 0, &live),
        AYNI_DENIED);
    EXPECT_EQ(session_info().state, AYNI_CONTROL_SESSION_IDLE);
    EXPECT_EQ(gate_info().phase, AYNI_CONTROL_IDLE);

    const auto proc_permit = make_permit(AYNI_FORM_PROCEDURE, 51, 10, 1000, -30, 30);
    const std::array<ayni_control_segment_v1, 2> good_pair{
        make_segment(10, 10, 100),
        make_segment(20, 20, 100),
    };
    EXPECT_EQ(
        ayni_control_session_open(session, gate, &proc_permit, good_pair.data(), 2, 10, &live),
        AYNI_INVALID_ARGUMENT);
    EXPECT_EQ(gate_info().phase, AYNI_CONTROL_IDLE);

    // Single-segment PROCEDURE opens cleanly, but refuses streaming mutations.
    ASSERT_EQ(
        ayni_control_session_open(session, gate, &proc_permit, good_pair.data(), 1, 10, &live),
        AYNI_OK);
    EXPECT_EQ(ayni_control_session_push_segment(session, &good_pair[1]), AYNI_INVALID_STATE);
    EXPECT_EQ(ayni_control_session_replace_target(session, &good_pair[1], 20), AYNI_INVALID_STATE);
}

} // namespace
