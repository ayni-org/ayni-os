#pragma once
#include "motor_plan.h"
#include <array>
#include <driver/pulse_cnt.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
namespace wifichat {
// Owns only GPIO/RMT/PCNT. The caller supplies authorization; no model or Core calls.
// Allocate this object in internal RAM: the RMT source buffers outlive transmission.
class MotorOutput {
  public:
    enum class Poll { running, completed, failed };
    // Boot/standby path: no RMT ownership exists yet. Establish low GPIO before LCD/Host.
    static esp_err_t coast_pins();
    static bool pins_disabled();
    esp_err_t initialize();
    esp_err_t start(const MotorCommand &command);
    // Seamless mid-session segment transition: reuses same-direction RMT channels without
    // teardown and drives reversed/coast pins low break-before-make (ADR-0112).
    esp_err_t transition(const MotorCommand &next);
    // Polls current segment completion while keeping RMT channels warm for transition().
    Poll poll_segment();
    // Abort only the in-flight waveform, then replace it under the SAME active permit.
    // Same-direction channels stay allocated; reversals/coast use detach_motor low readback.
    esp_err_t replace_active(const MotorCommand &next);
    struct ReplacementTiming {
        bool reverse_or_coast = false; // True when pin routing changes, not just duty.
        uint64_t gap_us = 0;           // Local SDK update duration; no physical wheel timing claim.
    };
    const ReplacementTiming &replacement_timing() const { return replacement_timing_; }
    esp_err_t stop();
    bool disabled() const; // Actual GPIO input/matrix/output readback, not shaft measurement.
    bool counts(std::array<int, 4> &out) const;
    const std::array<uint32_t, 4> &expected_counts() const { return expected_; }
    esp_err_t last_error() const { return last_error_; }
    esp_err_t close(); // Only after output stop; releases partial initialization too.

  private:
    static constexpr size_t block_symbols = motor_duration_step_ms;
    enum class State { cold, idle, running, fault, closed };
    enum class CounterState { none, created, enabled, running };
    struct Counter {
        pcnt_unit_handle_t unit = nullptr;
        pcnt_channel_handle_t channel = nullptr;
        CounterState state = CounterState::none;
    };
    struct Output {
        rmt_channel_handle_t channel = nullptr;
        rmt_encoder_handle_t encoder = nullptr;
        std::array<rmt_symbol_word_t, block_symbols> waveform{};
    };
    static void fill_waveform(Output &output, int32_t percent);
    esp_err_t detach_motor(size_t motor);
    esp_err_t prepare(size_t motor, int32_t percent);
    esp_err_t apply_segment(const MotorCommand &command);
    ReplacementTiming replacement_timing_{}; // Control-worker owned diagnostic of last hot update.
    State state_ = State::cold;
    esp_err_t last_error_ = ESP_OK;
    std::array<Counter, 4> counters_{};
    std::array<Output, 2> outputs_{};
    rmt_sync_manager_handle_t sync_ = nullptr;
    std::array<int32_t, 2> active_percent_{}; // Active signed percent per motor, 0 when coasting.
    uint64_t coast_until_us_ = 0;             // Nonzero only during an all-zero coast segment.
    std::array<uint32_t, 4> expected_{};      // Cumulative rising edges across started segments.
};
} // namespace wifichat
