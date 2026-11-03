// Manual isolated bench diagnostic, not an Agent capability or stop-chain acceptance.
// No output starts at boot: a fresh ARM permits exactly one bounded PULSE.
#include "motor_plan.h"
#include "protocol.h"
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <driver/pulse_cnt.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/pcnt_ll.h>
#include <hal/rmt_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
#include <soc/soc_caps.h>
#include <string_view>
namespace {
static_assert(SOC_RMT_SUPPORT_TX_LOOP_COUNT && SOC_RMT_SUPPORT_TX_LOOP_AUTO_STOP,
              "Probe requires finite hardware loop auto-stop, not ISR-only stopping");
static_assert(PCNT_LL_GET(UNITS_PER_INST) >= usbchat::motor_pins.size(),
              "Each motor input needs an independent hardware pulse counter");
constexpr uint32_t resolution_hz = 1000000, pwm_period_ticks = 1000;
constexpr uint32_t duty_max = 50, duration_min_ms = 50, duration_max_ms = 5000;
constexpr size_t waveform_symbols = 5;
// A five-millisecond block keeps even the five-second trial inside one finite
// hardware loop batch. No ISR has to reload or restart loops to extend motion.
static_assert(duration_max_ms % waveform_symbols == 0);
static_assert(duration_max_ms / waveform_symbols <= RMT_LL_MAX_LOOP_COUNT_PER_BATCH);
static_assert(waveform_symbols + 1 <= SOC_RMT_MEM_WORDS_PER_CHANNEL);
constexpr int64_t hello_interval_us = 1000000, arm_window_us = 2000000;
constexpr int64_t completion_wait_us = int64_t(duration_max_ms) * 1000 + 1000000;
constexpr size_t symbols_per_channel = 48, line_capacity = 128;
constexpr size_t no_active_pin = usbchat::motor_pins.size();
constexpr int counter_low_limit = -1, counter_high_limit = 32767;
static_assert(duration_max_ms < counter_high_limit);
constexpr int uart_baud = 921600;
constexpr uart_port_t port = UART_NUM_0;
std::array<rmt_channel_handle_t, usbchat::motor_pins.size()> channels{};
rmt_encoder_handle_t encoder = nullptr;
// Internal static storage stays alive until transmission ends.
std::array<rmt_symbol_word_t, waveform_symbols> waveform{};
bool armed = false, active = false, faulted = false;
unsigned long long arm_nonce = 0;
size_t active_pin = 0;
int64_t started_at = 0;
int64_t armed_at = 0;
uint32_t requested_cycles = 0;
// Created once at boot and retained until reset, including partial startup failure.
// Hardware counting stays on through RMT setup, transmission and release; no IRQ gap
// or input glitch filter is used. A failed startup latches the probe unavailable.
std::array<pcnt_unit_handle_t, usbchat::motor_pins.size()> counters{};
bool counters_ready = false;
bool setup_counters() {
    for (size_t at = 0; at < counters.size(); ++at) {
        pcnt_unit_config_t unit{};
        unit.clk_src = PCNT_CLK_SRC_DEFAULT;
        unit.low_limit = counter_low_limit;
        unit.high_limit = counter_high_limit;
        if (pcnt_new_unit(&unit, &counters[at]) != ESP_OK) {
            return false;
        }
        pcnt_chan_config_t config{};
        config.edge_gpio_num = static_cast<int>(usbchat::motor_pins[at]);
        config.level_gpio_num = -1;
        pcnt_channel_handle_t channel = nullptr;
        if (pcnt_new_channel(counters[at], &config, &channel) != ESP_OK ||
            pcnt_channel_set_edge_action(channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                         PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK ||
            pcnt_channel_set_level_action(channel, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                          PCNT_CHANNEL_LEVEL_ACTION_KEEP) != ESP_OK ||
            pcnt_unit_set_glitch_filter(counters[at], nullptr) != ESP_OK ||
            pcnt_unit_enable(counters[at]) != ESP_OK ||
            pcnt_unit_clear_count(counters[at]) != ESP_OK ||
            pcnt_unit_start(counters[at]) != ESP_OK) {
            return false;
        }
    }
    counters_ready = true;
    return true;
}
bool clear_counters() {
    for (auto counter : counters) {
        if (!counter || pcnt_unit_clear_count(counter) != ESP_OK) {
            return false;
        }
    }
    return true;
}
bool idle_edges() {
    for (auto counter : counters) {
        int count = 0;
        if (pcnt_unit_get_count(counter, &count) != ESP_OK || count != 0) {
            return false;
        }
    }
    return true;
}
bool expected_active_edges() {
    for (size_t at = 0; at < counters.size(); ++at) {
        int count = 0;
        if (pcnt_unit_get_count(counters[at], &count) != ESP_OK || count < 0 ||
            (at == active_pin ? uint32_t(count) > requested_cycles : count != 0)) {
            return false;
        }
    }
    return true;
}
bool edge_result() {
    bool ok = true;
    std::printf("EDGES");
    for (size_t at = 0; at < counters.size(); ++at) {
        int count = 0;
        const auto rc = pcnt_unit_get_count(counters[at], &count);
        const auto expected = at == active_pin ? requested_cycles : 0;
        ok = ok && rc == ESP_OK && count >= 0 && uint32_t(count) == expected;
        std::printf(" %lu=%d", static_cast<unsigned long>(usbchat::motor_pins[at]), count);
    }
    std::puts("");
    return ok;
}

bool routes(size_t selected = no_active_pin) {
    bool ok = true;
    std::array<gpio_io_config_t, usbchat::motor_pins.size()> configs{};
    std::array<int, usbchat::motor_pins.size()> levels{};
    for (size_t at = 0; at < usbchat::motor_pins.size(); ++at) {
        const auto pin = static_cast<gpio_num_t>(usbchat::motor_pins[at]);
        auto &config = configs[at];
        const auto rc = gpio_get_io_config(pin, &config);
        const int level = gpio_get_level(pin);
        levels[at] = level;
        const bool expected = at == selected ? config.sig_out >= RMT_SIG_OUT0_IDX &&
                                                   config.sig_out <= RMT_SIG_OUT3_IDX
                                             : config.sig_out == SIG_GPIO_OUT_IDX;
        ok = ok && rc == ESP_OK && config.fun_sel == PIN_FUNC_GPIO && config.ie && config.oe &&
             expected && level == 0;
    }
    if (!ok) {
        return false; // The caller must coast before logging an unexpected output level.
    }
    std::printf("ROUTES");
    for (size_t at = 0; at < usbchat::motor_pins.size(); ++at) {
        std::printf(" %lu:%lu:%d", static_cast<unsigned long>(usbchat::motor_pins[at]),
                    static_cast<unsigned long>(configs[at].sig_out), levels[at]);
    }
    std::puts("");
    return ok;
}

bool coast() {
    bool ok = true;
    for (size_t at = 0; at < channels.size(); ++at) {
        if (channels[at]) {
            const auto disabled = rmt_disable(channels[at]);
            const auto removed = (disabled == ESP_OK || disabled == ESP_ERR_INVALID_STATE)
                                     ? rmt_del_channel(channels[at])
                                     : disabled;
            if (removed == ESP_OK) {
                channels[at] = nullptr;
            } else {
                ok = false; // Keep the handle if the driver could still own it.
            }
        }
        const auto pin = static_cast<gpio_num_t>(usbchat::motor_pins[at]);
        const auto level = gpio_set_level(pin, 0);
        // gpio_reset_pin and RMT deletion do not detach the output matrix.
        // Explicitly select the GPIO latch before re-enabling any old pin.
        esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
        ok = ok && level == ESP_OK;
    }
    gpio_config_t config{};
    for (const auto pin : usbchat::motor_pins) {
        config.pin_bit_mask |= UINT64_C(1) << pin;
    }
    config.mode = GPIO_MODE_INPUT_OUTPUT;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    const auto configured = gpio_config(&config);
    const bool isolated = routes();
    ok = ok && configured == ESP_OK && isolated;
    active = armed = false;
    faulted = faulted || !ok;
    return ok;
}
bool setup_channel(size_t at) {
    rmt_tx_channel_config_t config{};
    config.gpio_num = static_cast<gpio_num_t>(usbchat::motor_pins[at]);
    config.clk_src = RMT_CLK_SRC_DEFAULT;
    config.resolution_hz = resolution_hz;
    config.mem_block_symbols = symbols_per_channel;
    config.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&config, &channels[at]) != ESP_OK) {
        return false;
    }
    return rmt_enable(channels[at]) == ESP_OK;
}
void fail(const char *reason) {
    const bool observed = counters_ready;
    coast();
    if (observed) {
        edge_result();
    }
    faulted = true;
    std::printf("ERROR %s; cut motor power; reset required\n", reason);
}
template <class T> bool number(std::string_view &source, T &out, int base = 10) {
    const auto separator = source.find(' ');
    const auto token = source.substr(0, separator);
    if (token.empty()) {
        return false;
    }
    const auto result = std::from_chars(token.data(), token.data() + token.size(), out, base);
    if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
        return false;
    }
    source =
        separator == std::string_view::npos ? std::string_view{} : source.substr(separator + 1);
    return true;
}
void command(const char *line) {
    if (!std::strcmp(line, "STOP")) {
        // The local switch remains the independent physical stop. USB is supplementary.
        if (coast() && (!counters_ready || expected_active_edges())) {
            std::puts("STOPPED outputs commanded low; shaft stop unverified");
        } else {
            fail("output disable failed");
        }
        return;
    }
    if (!std::strcmp(line, "CHECK_UNPOWERED") && !active && !faulted) {
        // Exercise real channel reuse with only idle-low levels, never a waveform.
        // Each old pin must return to its GPIO latch before the next pin is routed.
        requested_cycles = 0;
        for (size_t at = 0; at < usbchat::motor_pins.size(); ++at) {
            if (!clear_counters() || !setup_channel(at) || !routes(at) || !coast() ||
                !idle_edges()) {
                fail("pin routing check failed; no pulse sent");
                return;
            }
        }
        std::puts(
            "CHECKED all four routes isolated; setup/release edges=0; no waveform transmitted");
        return;
    }
    unsigned long long nonce = 0;
    unsigned channel = 0, direction = 0, duty = 0, duration = 0;
    const std::string_view input(line);
    auto fields = input.size() >= 4 ? input.substr(4) : std::string_view{};
    if (input.substr(0, 5) == "PING ") {
        fields = input.substr(5);
        if (!active && !faulted && number(fields, nonce, 16) && fields.empty()) {
            std::printf("PONG nonce=%llx; uptime_ms=%lld;\n", nonce,
                        static_cast<long long>(esp_timer_get_time() / 1000));
        } else {
            std::puts("REFUSED invalid status request or probe state");
        }
        return;
    }
    if (!active && !faulted && input.substr(0, 4) == "ARM " && number(fields, nonce, 16) &&
        fields.empty() && nonce > arm_nonce) {
        arm_nonce = nonce;
        armed = true;
        armed_at = esp_timer_get_time();
        std::puts("ARMED one pulse only");
        return;
    }
    fields = input.size() >= 6 ? input.substr(6) : std::string_view{};
    if (input.substr(0, 6) != "PULSE " || !number(fields, nonce, 16) || !number(fields, channel) ||
        !number(fields, direction) || !number(fields, duty) || !number(fields, duration) ||
        !fields.empty() || !armed || active || faulted || nonce != arm_nonce ||
        esp_timer_get_time() - armed_at >= arm_window_us || !channel || channel > 2 ||
        direction > 1 || !duty || duty > duty_max || duration < duration_min_ms ||
        duration > duration_max_ms || duration % waveform_symbols != 0) {
        std::puts("REFUSED invalid/consumed arm or pulse bounds");
        return;
    }
    armed = false; // Consume before any hardware API; never retry an uncertain pulse.
    active_pin = (channel - 1) * 2 + direction;
    requested_cycles = 0;
    if (!clear_counters() || !setup_channel(active_pin) || !routes(active_pin) || !idle_edges()) {
        fail("RMT setup failed or an edge appeared before transmission");
        return;
    }
    rmt_symbol_word_t pulse{};
    pulse.level0 = 1;
    pulse.duration0 = pwm_period_ticks * duty / usbchat::motor_percent_scale;
    pulse.level1 = 0;
    pulse.duration1 = pwm_period_ticks - pulse.duration0;
    rmt_transmit_config_t tx{};
    waveform.fill(pulse);
    tx.loop_count = static_cast<int>(duration / waveform_symbols);
    tx.flags.eot_level = 0;
    tx.flags.queue_nonblocking = true;
    requested_cycles = duration;
    started_at = esp_timer_get_time();
    if (rmt_transmit(channels[active_pin], encoder, waveform.data(), sizeof(waveform), &tx) !=
        ESP_OK) {
        fail("RMT submit failed, outcome unknown");
        return;
    }
    active = true;
    std::printf("ACCEPTED nonce=%llx channel=%u direction=%u duty=%u duration_ms=%u\n", nonce,
                channel, direction, duty, duration);
}
void checked_command(char *line) {
    // Plain STOP can only remove output and remains available even on framing errors.
    if (!std::strcmp(line, "STOP")) {
        command(line);
        return;
    }
    auto *marker = std::strrchr(line, '*');
    uint32_t crc = 0;
    if (!marker || marker == line || marker[-1] != ' ' || std::strlen(marker + 1) != 8) {
        std::puts("REFUSED missing command checksum");
        return;
    }
    const auto parsed = std::from_chars(marker + 1, marker + 9, crc, 16);
    const auto bytes = size_t(marker - line - 1);
    if (parsed.ec != std::errc{} || parsed.ptr != marker + 9 ||
        usbchat::checksum(reinterpret_cast<const uint8_t *>(line), bytes) != crc) {
        std::puts("REFUSED command checksum mismatch");
        return;
    }
    line[bytes] = 0;
    command(line);
}
} // namespace
extern "C" void app_main() {
    coast();
    rmt_copy_encoder_config_t copy{};
    if (rmt_new_copy_encoder(&copy, &encoder) != ESP_OK) {
        fail("encoder allocation failed");
    }
    uart_config_t config{};
    config.baud_rate = uart_baud;
    config.data_bits = UART_DATA_8_BITS;
    config.parity = UART_PARITY_DISABLE;
    config.stop_bits = UART_STOP_BITS_1;
    config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    config.source_clk = UART_SCLK_DEFAULT;
    if (uart_param_config(port, &config) != ESP_OK ||
        uart_driver_install(port, 1024, 0, 0, nullptr, 0) != ESP_OK) {
        coast();
        return;
    }
    if (!setup_counters()) {
        fail("hardware GPIO counter allocation failed");
    }
    char line[line_capacity]{};
    size_t used = 0;
    bool discard = false;
    int64_t hello_at = 0;
    for (;;) {
        const auto now = esp_timer_get_time();
        if (!active && now - hello_at >= hello_interval_us) {
            if (faulted) {
                std::puts("ERROR output unavailable; cut motor power; reset required");
            } else {
                // Opening a serial device can change modem-control lines in its OS driver.
                // Reporting uptime lets a power-off check detect an unexpected reset on open.
                std::printf("AYNI MOTOR PROBE/8; D0..D3=38,39,40,41; CRC and full-phase PCNT "
                            "checks; uptime_ms=%lld; manual bench only\n",
                            static_cast<long long>(now / 1000));
            }
            hello_at = now;
        }
        uint8_t byte = 0;
        if (uart_read_bytes(port, &byte, 1, pdMS_TO_TICKS(1)) == 1) {
            if (byte == '\n') {
                line[used] = 0;
                if (!discard) {
                    checked_command(line);
                }
                used = 0;
                discard = false;
            } else if (byte != '\r') {
                if (byte == 0 || used + 1 >= sizeof(line)) {
                    discard = true;
                } else if (!discard) {
                    line[used++] = char(byte);
                }
            }
        }
        if (active) {
            if (!expected_active_edges()) {
                fail("GPIO hardware count or readback mismatch");
                continue;
            }
            const auto done = rmt_tx_wait_all_done(channels[active_pin], 0);
            if (done == ESP_OK) {
                const bool stopped = coast();
                const bool measured = edge_result();
                if (measured && stopped) {
                    std::puts(
                        "DONE finite waveform ended; outputs commanded low; rotation unverified");
                } else {
                    fail("GPIO edge feedback or output disable failed");
                }
            } else if (done != ESP_ERR_TIMEOUT || now - started_at > completion_wait_us) {
                fail("transmission completion unknown");
            }
        }
    }
}
