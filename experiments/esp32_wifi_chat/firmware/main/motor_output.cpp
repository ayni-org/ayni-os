#include "motor_output.h"
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
#include <soc/rmt_reg.h>
#include <soc/soc_caps.h>
namespace wifichat {
namespace {
constexpr uint32_t resolution_hz = 1000000, period_ticks = 1000;
constexpr int counter_low = -1, counter_high = 32767;
constexpr size_t channel_symbols = 48;
static_assert(SOC_RMT_SUPPORT_TX_LOOP_COUNT && SOC_RMT_SUPPORT_TX_LOOP_AUTO_STOP);
// IDF 6.0.3 batches finite loops above 1023 and auto-stops each hardware batch.
// PCNT extends its signed hardware counter through limit interrupts.
static_assert(motor_plan_duration_ms_max <= INT32_MAX);
void retain_error(esp_err_t &first, esp_err_t next) {
    if (first == ESP_OK && next != ESP_OK) {
        first = next;
    }
}
} // namespace
esp_err_t MotorOutput::initialize() {
    if (state_ != State::cold) {
        return ESP_ERR_INVALID_STATE;
    }
    auto rc = stop();
    for (size_t at = 0; rc == ESP_OK && at < counters_.size(); ++at) {
        auto &counter = counters_[at];
        pcnt_unit_config_t config{};
        config.clk_src = PCNT_CLK_SRC_DEFAULT;
        config.low_limit = counter_low;
        config.high_limit = counter_high;
        config.flags.accum_count = true;
        rc = pcnt_new_unit(&config, &counter.unit);
        if (rc != ESP_OK) {
            break;
        }
        counter.state = CounterState::created;
        pcnt_chan_config_t channel{};
        channel.edge_gpio_num = static_cast<int>(motor_pins[at]);
        channel.level_gpio_num = -1;
        rc = pcnt_new_channel(counter.unit, &channel, &counter.channel);
        if (rc == ESP_OK) {
            rc = pcnt_channel_set_edge_action(counter.channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                              PCNT_CHANNEL_EDGE_ACTION_HOLD);
        }
        if (rc == ESP_OK) {
            rc = pcnt_channel_set_level_action(counter.channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                               PCNT_CHANNEL_LEVEL_ACTION_KEEP);
        }
        if (rc == ESP_OK) {
            rc = pcnt_unit_set_glitch_filter(counter.unit, nullptr);
        }
        if (rc == ESP_OK) {
            rc = pcnt_unit_add_watch_point(counter.unit, counter_high);
        }
        if (rc == ESP_OK) {
            rc = pcnt_unit_add_watch_point(counter.unit, counter_low);
        }
        if (rc == ESP_OK && (rc = pcnt_unit_enable(counter.unit)) == ESP_OK) {
            counter.state = CounterState::enabled;
            rc = pcnt_unit_clear_count(counter.unit);
        }
        if (rc == ESP_OK && (rc = pcnt_unit_start(counter.unit)) == ESP_OK) {
            counter.state = CounterState::running;
        }
    }
    for (auto &output : outputs_) {
        if (rc == ESP_OK) {
            rmt_copy_encoder_config_t copy{};
            rc = rmt_new_copy_encoder(&copy, &output.encoder);
        }
    }
    if (rc != ESP_OK) {
        close();
        // Keep the initialization cause even if partial-resource cleanup also reports an error.
        last_error_ = rc;
        state_ = State::fault;
        return rc;
    }
    state_ = State::idle;
    return ESP_OK;
}
bool MotorOutput::pins_disabled() {
    for (const auto pin : motor_pins) {
        gpio_io_config_t config{};
        if (gpio_get_io_config(static_cast<gpio_num_t>(pin), &config) != ESP_OK ||
            config.fun_sel != PIN_FUNC_GPIO || config.sig_out != SIG_GPIO_OUT_IDX || !config.ie ||
            !config.oe || gpio_get_level(static_cast<gpio_num_t>(pin)) != 0) {
            return false;
        }
    }
    return true;
}
bool MotorOutput::disabled() const {
    return pins_disabled();
}
esp_err_t MotorOutput::coast_pins() {
    esp_err_t rc = ESP_OK;
    gpio_config_t config{};
    for (auto pin : motor_pins) {
        retain_error(rc, gpio_set_level(static_cast<gpio_num_t>(pin), 0));
        esp_rom_gpio_connect_out_signal(static_cast<gpio_num_t>(pin), SIG_GPIO_OUT_IDX, false,
                                        false);
        config.pin_bit_mask |= UINT64_C(1) << pin;
    }
    config.mode = GPIO_MODE_INPUT_OUTPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    retain_error(rc, gpio_config(&config));
    if (!pins_disabled()) {
        retain_error(rc, ESP_FAIL);
    }
    return rc;
}
bool MotorOutput::counts(std::array<int, 4> &out) const {
    for (size_t at = 0; at < counters_.size(); ++at) {
        if (!counters_[at].unit || pcnt_unit_get_count(counters_[at].unit, &out[at]) != ESP_OK ||
            out[at] < 0) {
            return false;
        }
    }
    return true;
}
void MotorOutput::fill_waveform(Output &output, int32_t percent) {
    const auto magnitude = motor_duty_percent(percent);
    rmt_symbol_word_t symbol{};
    symbol.level0 = 1;
    symbol.duration0 = period_ticks * magnitude / motor_percent_scale;
    symbol.duration1 = period_ticks - symbol.duration0;
    output.waveform.fill(symbol);
}
// Detaches one motor's RMT channel and forces both of its H-bridge inputs low before any
// opposite-polarity channel can attach (TA6586 break-before-make, ADR-0112).
esp_err_t MotorOutput::detach_motor(size_t motor) {
    esp_err_t rc = ESP_OK;
    auto &output = outputs_[motor];
    if (output.channel) {
        const auto dis = rmt_disable(output.channel);
        retain_error(rc, dis == ESP_ERR_INVALID_STATE ? ESP_OK : dis);
    }
    gpio_config_t config{};
    for (size_t dir = 0; dir < motor_inputs_per_channel; ++dir) {
        const auto pin = motor_pins[motor * motor_inputs_per_channel + dir];
        retain_error(rc, gpio_set_level(static_cast<gpio_num_t>(pin), 0));
        esp_rom_gpio_connect_out_signal(static_cast<gpio_num_t>(pin), SIG_GPIO_OUT_IDX, false,
                                        false);
        config.pin_bit_mask |= UINT64_C(1) << pin;
    }
    config.mode = GPIO_MODE_INPUT_OUTPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    retain_error(rc, gpio_config(&config));
    if (output.channel) {
        const auto removed = rmt_del_channel(output.channel);
        retain_error(rc, removed);
        if (removed == ESP_OK) {
            output.channel = nullptr;
        }
    }
    for (size_t dir = 0; dir < motor_inputs_per_channel; ++dir) {
        const auto pin = motor_pins[motor * motor_inputs_per_channel + dir];
        if (gpio_get_level(static_cast<gpio_num_t>(pin)) != 0) {
            retain_error(rc, ESP_FAIL);
        }
    }
    active_percent_[motor] = 0;
    return rc;
}
esp_err_t MotorOutput::prepare(size_t motor, int32_t percent) {
    if (percent == 0) {
        return ESP_OK;
    }
    auto &output = outputs_[motor];
    const size_t input = motor_input(motor, percent);
    fill_waveform(output, percent);
    rmt_tx_channel_config_t config{};
    config.gpio_num = static_cast<gpio_num_t>(motor_pins[input]);
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = resolution_hz;
    config.mem_block_symbols = channel_symbols;
    config.trans_queue_depth = 1;
    auto rc = rmt_new_tx_channel(&config, &output.channel);
    if (rc == ESP_OK) {
        rc = rmt_enable(output.channel);
    }
    return rc;
}
esp_err_t MotorOutput::apply_segment(const MotorCommand &command) {
    const std::array<int32_t, 2> next{command.first_percent, command.second_percent};
    std::array<bool, 2> reuse{};
    bool topology_changed = false;
    for (size_t m = 0; m < outputs_.size(); ++m) {
        const bool was_active = active_percent_[m] != 0 && outputs_[m].channel != nullptr;
        const bool now_active = next[m] != 0;
        reuse[m] = was_active && now_active &&
                   motor_input(m, active_percent_[m]) == motor_input(m, next[m]);
        topology_changed =
            topology_changed || (was_active != now_active) || (now_active && !reuse[m]);
    }
    esp_err_t rc = ESP_OK;
    if (sync_ && topology_changed) {
        const auto removed = rmt_del_sync_manager(sync_);
        retain_error(rc, removed);
        if (removed == ESP_OK) {
            sync_ = nullptr;
        }
    }
    for (size_t m = 0; rc == ESP_OK && m < outputs_.size(); ++m) {
        if (reuse[m]) {
            fill_waveform(outputs_[m], next[m]);
        } else {
            if (outputs_[m].channel) {
                rc = detach_motor(m);
            }
            if (rc == ESP_OK && next[m] != 0) {
                rc = prepare(m, next[m]);
            }
        }
    }
    if (rc == ESP_OK && outputs_[0].channel && outputs_[1].channel) {
        if (sync_) {
            rc = rmt_sync_reset(sync_);
        } else {
            rmt_channel_handle_t channels[]{outputs_[0].channel, outputs_[1].channel};
            rmt_sync_manager_config_t config{channels, 2};
            rc = rmt_new_sync_manager(&config, &sync_);
        }
    }
    if (rc == ESP_OK && !outputs_[0].channel && !outputs_[1].channel) {
        if (!pins_disabled()) {
            rc = ESP_FAIL;
        } else {
            coast_until_us_ =
                uint64_t(esp_timer_get_time()) + uint64_t(command.duration_ms) * 1000ULL;
        }
    } else {
        coast_until_us_ = 0;
        for (size_t m = 0; rc == ESP_OK && m < outputs_.size(); ++m) {
            if (!outputs_[m].channel) {
                continue;
            }
            const size_t input = motor_input(m, next[m]);
            expected_[input] += command.duration_ms;
            rmt_transmit_config_t tx{};
            tx.loop_count = command.duration_ms / motor_duration_step_ms;
            tx.flags.eot_level = 0;
            tx.flags.queue_nonblocking = true;
            rc = rmt_transmit(outputs_[m].channel, outputs_[m].encoder, outputs_[m].waveform.data(),
                              sizeof(outputs_[m].waveform), &tx);
        }
    }
    if (rc != ESP_OK) {
        stop();
        last_error_ = rc;
        state_ = State::fault;
        return rc;
    }
    active_percent_ = next;
    return ESP_OK;
}
esp_err_t MotorOutput::start(const MotorCommand &command) {
    if (state_ != State::idle || !disabled()) {
        last_error_ = ESP_ERR_INVALID_STATE;
        return last_error_;
    }
    last_error_ = ESP_OK;
    if (!valid_motor_waveform(command)) {
        return ESP_ERR_INVALID_ARG;
    }
    expected_ = {};
    active_percent_ = {};
    coast_until_us_ = 0;
    auto rc = ESP_OK;
    for (auto &counter : counters_) {
        retain_error(rc, pcnt_unit_clear_count(counter.unit));
    }
    std::array<int, 4> idle{};
    if (rc == ESP_OK && (!counts(idle) || idle != std::array<int, 4>{})) {
        rc = ESP_FAIL;
    }
    if (rc != ESP_OK) {
        stop();
        last_error_ = rc;
        state_ = State::fault;
        return rc;
    }
    state_ = State::running;
    return apply_segment(command);
}
esp_err_t MotorOutput::transition(const MotorCommand &next) {
    if (state_ != State::running) {
        last_error_ = ESP_ERR_INVALID_STATE;
        return last_error_;
    }
    if (!valid_motor_waveform(next)) {
        return ESP_ERR_INVALID_ARG;
    }
    return apply_segment(next);
}
// RMT borrows waveform memory until completion/disable. Never patch a live buffer.
// IDF 6.0.3 rmt_tx_disable synchronously stops TX and returns its interrupted descriptor.
esp_err_t MotorOutput::replace_active(const MotorCommand &next) {
    if (state_ != State::running || !valid_motor_waveform(next)) {
        return state_ == State::running ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
    }
    const auto began = uint64_t(esp_timer_get_time());
    const std::array<int32_t, 2> wanted{next.first_percent, next.second_percent};
    bool reverse_or_coast = false;
    esp_err_t rc = ESP_OK;
    for (size_t at = 0; at < outputs_.size(); ++at) {
        reverse_or_coast = reverse_or_coast || (active_percent_[at] == 0) != (wanted[at] == 0) ||
                           (active_percent_[at] && wanted[at] &&
                            motor_input(at, active_percent_[at]) != motor_input(at, wanted[at]));
        if (outputs_[at].channel) {
            retain_error(rc, rmt_disable(outputs_[at].channel));
        }
    }
    std::array<int, 4> measured{};
    if (rc == ESP_OK && !counts(measured)) {
        rc = ESP_FAIL;
    }
    if (rc == ESP_OK) {
        // The old finite transaction was deliberately truncated. Its actual PCNT count is the
        // new baseline; it must not be counted as an exactly completed trajectory segment.
        for (size_t at = 0; at < expected_.size(); ++at) {
            expected_[at] = uint32_t(measured[at]);
        }
        for (size_t at = 0; at < outputs_.size(); ++at) {
            auto &output = outputs_[at];
            // Reversing/coasting channels remain disabled until detach_motor removes them.
            // Only same-direction channels are reused by apply_segment.
            if (output.channel && wanted[at] && active_percent_[at] &&
                motor_input(at, wanted[at]) == motor_input(at, active_percent_[at])) {
                retain_error(rc, rmt_enable(output.channel));
            }
        }
    }
    if (rc == ESP_OK) {
        rc = apply_segment(next);
    }
    if (rc != ESP_OK) {
        stop();
        last_error_ = rc;
        state_ = State::fault;
    }
    replacement_timing_ = {reverse_or_coast, uint64_t(esp_timer_get_time()) - began};
    return rc;
}
MotorOutput::Poll MotorOutput::poll_segment() {
    if (state_ != State::running) {
        return Poll::failed;
    }
    std::array<int, 4> observed{};
    bool valid = counts(observed), done = true;
    for (size_t at = 0; at < observed.size(); ++at) {
        valid = valid && uint32_t(observed[at]) <= expected_[at];
    }
    if (coast_until_us_ != 0) {
        done = uint64_t(esp_timer_get_time()) >= coast_until_us_;
    } else {
        for (const auto &output : outputs_) {
            if (!output.channel) {
                continue;
            }
            const auto rc = rmt_tx_wait_all_done(output.channel, 0);
            valid = valid && (rc == ESP_OK || rc == ESP_ERR_TIMEOUT);
            done = done && rc == ESP_OK;
        }
    }
    if (!valid) {
        stop();
        last_error_ = ESP_FAIL;
        state_ = State::fault;
        return Poll::failed;
    }
    if (!done) {
        return Poll::running;
    }
    coast_until_us_ = 0;
    valid = counts(observed);
    for (size_t at = 0; at < observed.size(); ++at) {
        valid = valid && uint32_t(observed[at]) == expected_[at];
    }
    if (!valid) {
        stop();
        if (last_error_ == ESP_OK) {
            last_error_ = ESP_FAIL;
        }
        state_ = State::fault;
        return Poll::failed;
    }
    return Poll::completed;
}
esp_err_t MotorOutput::stop() {
    active_percent_ = {};
    coast_until_us_ = 0;
    esp_err_t rc = ESP_OK;
    for (auto &output : outputs_) {
        if (output.channel) {
            const auto disabled = rmt_disable(output.channel);
            // INIT channels never started, so INVALID_STATE already means disabled.
            retain_error(rc, disabled == ESP_ERR_INVALID_STATE ? ESP_OK : disabled);
        }
    }
    // Detach before deleting the last RMT channel can power down its shared peripheral.
    for (auto pin : motor_pins) {
        retain_error(rc, gpio_set_level(static_cast<gpio_num_t>(pin), 0));
        esp_rom_gpio_connect_out_signal(static_cast<gpio_num_t>(pin), SIG_GPIO_OUT_IDX, false,
                                        false);
    }
    if (sync_) {
        const auto removed = rmt_del_sync_manager(sync_);
        retain_error(rc, removed);
        if (removed == ESP_OK) {
            sync_ = nullptr;
        }
    }
    for (auto &output : outputs_) {
        if (output.channel && !sync_) {
            const auto removed = rmt_del_channel(output.channel);
            retain_error(rc, removed);
            if (removed == ESP_OK) {
                output.channel = nullptr;
            }
        }
    }
    retain_error(rc, coast_pins());
    if (rc != ESP_OK) {
        last_error_ = rc;
        state_ = State::fault;
    } else if (state_ != State::fault && state_ != State::closed) {
        state_ = State::idle;
    }
    return rc;
}
esp_err_t MotorOutput::close() {
    auto rc = stop();
    for (auto &counter : counters_) {
        if (counter.state == CounterState::running) {
            const auto stopped = pcnt_unit_stop(counter.unit);
            retain_error(rc, stopped);
            if (stopped == ESP_OK) {
                counter.state = CounterState::enabled;
            }
        }
        if (counter.state == CounterState::enabled) {
            const auto disabled = pcnt_unit_disable(counter.unit);
            retain_error(rc, disabled);
            if (disabled == ESP_OK) {
                counter.state = CounterState::created;
            }
        }
        if (counter.state == CounterState::created && counter.channel) {
            const auto removed = pcnt_del_channel(counter.channel);
            retain_error(rc, removed);
            if (removed == ESP_OK) {
                counter.channel = nullptr;
            }
        }
        if (counter.state == CounterState::created && !counter.channel) {
            const auto removed = pcnt_del_unit(counter.unit);
            retain_error(rc, removed);
            if (removed == ESP_OK) {
                counter = {};
            }
        }
    }
    for (auto &output : outputs_) {
        if (output.encoder && !output.channel) {
            const auto removed = rmt_del_encoder(output.encoder);
            retain_error(rc, removed);
            if (removed == ESP_OK) {
                output.encoder = nullptr;
            }
        }
    }
    state_ = rc == ESP_OK ? State::closed : State::fault;
    return rc;
}
} // namespace wifichat
