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
    Poll poll();
    esp_err_t stop();
    bool disabled() const; // Actual GPIO input/matrix/output readback, not shaft measurement.
    bool counts(std::array<int, 4> &out) const;
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
    esp_err_t prepare(size_t motor, int32_t percent);
    State state_ = State::cold;
    esp_err_t last_error_ = ESP_OK;
    std::array<Counter, 4> counters_{};
    std::array<Output, 2> outputs_{};
    rmt_sync_manager_handle_t sync_ = nullptr;
    std::array<uint32_t, 4> expected_{}; // Rising edges, set before any per-command GPIO work.
};
} // namespace wifichat
