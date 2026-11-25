// Executor-owned state only. No Kernel, Provider, resource-lock or hardware dependency.
#ifndef AYNI_CONTROL_PERMIT_GATE_H
#define AYNI_CONTROL_PERMIT_GATE_H
#include <array>
#include <ayni/control.h>
#include <optional>

namespace ayni::control {
// Catalog-name syntax/order shared by envelopes, effective samples and loop configuration.
bool valid_name(const ayni_catalog_name_v1 &name);
int compare_names(const ayni_catalog_name_v1 &left, const ayni_catalog_name_v1 &right);
// Canonical numeric envelopes are shared by config and permit validation.
ayni_status_t normalize_envelope(const ayni_control_envelope_v1 &input,
                                 ayni_control_envelope_v1 &out);
// Exact field/unit sets; approved bounds must fit every measured device range.
bool within(const ayni_control_envelope_v1 &approved, const ayni_control_envelope_v1 &limits);
bool valid_epochs(const ayni_permit_epochs_v1 &epochs);
bool same_epochs(const ayni_permit_epochs_v1 &left, const ayni_permit_epochs_v1 &right);
bool valid_sample(const ayni_control_sample_v1 &sample, const ayni_execution_permit_v1 &permit);
ayni_status_t structure(uint32_t actual, uint32_t expected);
} // namespace ayni::control

// Caller arena owns this executor's complete fixed-size state until explicit destroy.
struct ayni_control_gate {
    struct Nonce {
        uint64_t id = 0;     // Zero is an unused cell; positive IDs never wrap/repeat.
        uint64_t issued = 0; // Executor-local microseconds, meaningful iff id != 0.
    };
    enum class Clock { valid, regressed };
    ayni_control_config_v1 config; // Immutable trusted limits/owner generations.
    struct Sample {
        uint64_t sequence; // Positive accepted sequence, coupled with its capture time.
        uint64_t at_us;    // Executor-local capture time; zero is a valid time.
    };
    struct Stop {
        ayni_control_stop_cause_t cause;      // First cause, never NONE.
        std::optional<uint64_t> confirmed_at; // Empty until fresh local stop measurement.
    };
    struct Execution {
        ayni_execution_permit_v1 permit; // Sole copy of accepted identity and full envelope.
        uint64_t deadline_us;            // Nonce-anchored lease deadline.
        std::optional<Sample> sample;    // Empty until first accepted command.
        std::optional<Stop> stopped;     // Empty while active; terminal once present.
    };
    std::optional<Execution>
        execution; // Empty iff IDLE; keeps terminal identity for replay rejection.
    std::array<Nonce, AYNI_CONTROL_NONCE_COUNT> nonces{}; // Fixed recent challenge window.
    uint64_t next_nonce = 0;    // Last issued ID; UINT64_MAX refuses new challenges.
    uint64_t last_now;          // Highest observed local microsecond time.
    Clock clock = Clock::valid; // Regression forbids new output until recreation.

    ayni_control_gate(const ayni_control_config_v1 &configured, uint64_t now);
    ayni_control_phase_t phase() const;    // Derived from explicit execution/stop facts.
    ayni_control_info_v1 snapshot() const; // Copied ABI projection, no duplicate state.
    // Time/observation evaluation may make a live permit terminal, never resurrect it.
    ayni_status_t tick(uint64_t now);
    ayni_status_t observe(uint64_t now, const ayni_control_observation_v1 *live);
    // First stop cause wins; confirmation is a separate trusted local fact.
    void stop(ayni_control_stop_cause_t cause);
    // A lease always starts at a retained local nonce, never at receipt time.
    ayni_status_t deadline(uint64_t nonce, uint32_t lease, uint64_t now, uint64_t &out) const;
    // Fresh permit identity and exact generations/mode plus bounded copied envelope.
    ayni_status_t prepare(const ayni_execution_permit_v1 &input,
                          ayni_execution_permit_v1 &out) const;
};
#endif
