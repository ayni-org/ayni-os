// Manual isolated bench diagnostic, not an Agent capability or stop-chain acceptance.
// No output starts at boot: a fresh ARM permits exactly one bounded PULSE.
#include "motor_plan.h"
#include "protocol.h"
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <driver/uart.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <soc/gpio_sig_map.h>
#include <soc/soc_caps.h>
#include <string_view>
namespace {
static_assert(SOC_RMT_SUPPORT_TX_LOOP_COUNT && SOC_RMT_SUPPORT_TX_LOOP_AUTO_STOP,
              "Probe requires finite hardware loop auto-stop, not ISR-only stopping");
constexpr uint32_t resolution_hz = 1000000, pwm_period_ticks = 1000;
constexpr uint32_t duty_max = 20, duration_min_ms = 50, duration_max_ms = 300;
constexpr int64_t hello_interval_us = 1000000, arm_window_us = 2000000;
constexpr int64_t completion_wait_us = 1000000;
constexpr size_t symbols_per_channel = 48, line_capacity = 128;
constexpr size_t no_active_pin = usbchat::motor_pins.size();
constexpr int uart_baud = 921600;
constexpr uart_port_t port = UART_NUM_0;
std::array<rmt_channel_handle_t, usbchat::motor_pins.size()> channels{};
rmt_encoder_handle_t encoder = nullptr;
rmt_symbol_word_t pulse{}; // Internal static storage stays alive until transmission ends.
bool armed = false, active = false, faulted = false;
unsigned long long arm_nonce = 0;
size_t active_pin = 0;
int64_t started_at = 0;
int64_t armed_at = 0;

bool routes(size_t selected = no_active_pin) {
    bool ok = true;
    std::printf("ROUTES");
    for (size_t at = 0; at < usbchat::motor_pins.size(); ++at) {
        const auto pin = static_cast<gpio_num_t>(usbchat::motor_pins[at]);
        gpio_io_config_t config{};
        const auto rc = gpio_get_io_config(pin, &config);
        const int level = gpio_get_level(pin);
        const bool expected = at == selected ? config.sig_out >= RMT_SIG_OUT0_IDX &&
                                                   config.sig_out <= RMT_SIG_OUT3_IDX
                                             : config.sig_out == SIG_GPIO_OUT_IDX;
        ok = ok && rc == ESP_OK && config.ie && config.oe && expected && level == 0;
        std::printf(" %lu:%lu:%d", static_cast<unsigned long>(pin),
                    static_cast<unsigned long>(config.sig_out), level);
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
    coast();
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
        if (coast()) {
            std::puts("STOPPED outputs commanded low; shaft stop unverified");
        } else {
            fail("output disable failed");
        }
        return;
    }
    if (!std::strcmp(line, "CHECK") && !active && !faulted) {
        // Exercise real channel reuse with only idle-low levels, never a waveform.
        // Each old pin must return to its GPIO latch before the next pin is routed.
        for (size_t at = 0; at < usbchat::motor_pins.size(); ++at) {
            if (!setup_channel(at) || !routes(at) || !coast()) {
                fail("pin routing check failed; no pulse sent");
                return;
            }
        }
        std::puts("CHECKED all four routes isolated; no waveform transmitted");
        return;
    }
    unsigned long long nonce = 0;
    unsigned channel = 0, direction = 0, duty = 0, duration = 0;
    const std::string_view input(line);
    auto fields = input.size() >= 4 ? input.substr(4) : std::string_view{};
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
        duration > duration_max_ms) {
        std::puts("REFUSED invalid/consumed arm or pulse bounds");
        return;
    }
    armed = false; // Consume before any hardware API; never retry an uncertain pulse.
    active_pin = (channel - 1) * 2 + direction;
    if (!setup_channel(active_pin) || !routes(active_pin)) {
        fail("RMT setup failed");
        return;
    }
    pulse = {};
    pulse.level0 = 1;
    pulse.duration0 = pwm_period_ticks * duty / usbchat::motor_percent_max;
    pulse.level1 = 0;
    pulse.duration1 = pwm_period_ticks - pulse.duration0;
    rmt_transmit_config_t tx{};
    tx.loop_count = duration; // One 1ms symbol per loop; S3 stops loops in hardware.
    tx.flags.eot_level = 0;
    tx.flags.queue_nonblocking = true;
    started_at = esp_timer_get_time();
    if (rmt_transmit(channels[active_pin], encoder, &pulse, sizeof(pulse), &tx) != ESP_OK) {
        fail("RMT submit failed, outcome unknown");
        return;
    }
    active = true;
    std::printf("ACCEPTED channel=%u direction=%u duty=%u duration_ms=%u\n", channel, direction,
                duty, duration);
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
    char line[line_capacity]{};
    size_t used = 0;
    bool discard = false;
    int64_t hello_at = 0;
    for (;;) {
        const auto now = esp_timer_get_time();
        if (!active && now - hello_at >= hello_interval_us) {
            std::puts(faulted
                          ? "ERROR output unavailable; cut motor power; reset required"
                          : "AYNI MOTOR PROBE/2; CRC required; default coast; manual bench only");
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
            const auto done = rmt_tx_wait_all_done(channels[active_pin], 0);
            if (done == ESP_OK) {
                if (coast()) {
                    std::puts(
                        "DONE finite waveform ended; outputs commanded low; rotation unverified");
                } else {
                    fail("output disable failed");
                }
            } else if (done != ESP_ERR_TIMEOUT || now - started_at > completion_wait_us) {
                fail("transmission completion unknown");
            }
        }
    }
}
