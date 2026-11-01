// Manual isolated bench diagnostic, not an Agent capability or stop-chain acceptance.
// No output starts at boot: a fresh ARM permits exactly one bounded PULSE.
#include "motor_plan.h"
#include "protocol.h"
#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <driver/gpio.h>
#include <driver/rmt_encoder.h>
#include <driver/rmt_tx.h>
#include <driver/uart.h>
#include <esp_attr.h>
#include <esp_intr_alloc.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/gpio_ll.h>
#include <sdkconfig.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
#include <soc/soc_caps.h>
#include <string_view>
namespace {
static_assert(SOC_RMT_SUPPORT_TX_LOOP_COUNT && SOC_RMT_SUPPORT_TX_LOOP_AUTO_STOP,
              "Probe requires finite hardware loop auto-stop, not ISR-only stopping");
static_assert(CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0,
              "GPIO observer and main task must share the configured owner core");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "ISR counters require inline atomics");
constexpr uint32_t resolution_hz = 1000000, pwm_period_ticks = 1000;
constexpr uint32_t duty_max = 20, duration_min_ms = 50, duration_max_ms = 300;
constexpr int64_t hello_interval_us = 1000000, arm_window_us = 2000000;
constexpr int64_t completion_wait_us = 1000000;
constexpr size_t symbols_per_channel = 48, line_capacity = 128;
constexpr size_t no_active_pin = usbchat::motor_pins.size();
constexpr uint32_t gpio_bank_width = 32, edge_tolerance = 1;
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
uint32_t requested_cycles = 0;
// Internal static RAM; the IRAM ISR only counts local pad edges, never performs I/O or logging.
DRAM_ATTR std::atomic<uint32_t> observed_pin{no_active_pin}, unexpected_edges{0};
DRAM_ATTR std::array<std::atomic<uint32_t>, usbchat::motor_pins.size()> rising_edges{};
void IRAM_ATTR rising(void *argument) {
    const auto expected = observed_pin.load(std::memory_order_relaxed);
    if (expected >= no_active_pin) {
        return;
    }
    const auto at = reinterpret_cast<uintptr_t>(argument);
    rising_edges[at].fetch_add(1, std::memory_order_relaxed);
    if (at != expected) {
        unexpected_edges.fetch_or(uint32_t(1) << at, std::memory_order_relaxed);
    }
}
bool stop_observing() {
    observed_pin.store(no_active_pin, std::memory_order_relaxed);
    bool ok = true;
    for (const auto pin : usbchat::motor_pins) {
        const auto rc = gpio_intr_disable(static_cast<gpio_num_t>(pin));
        ok = ok && rc == ESP_OK;
    }
    return ok;
}
bool start_observing(size_t selected) {
    if (!stop_observing()) {
        return false;
    }
    uint64_t mask = 0;
    for (size_t at = 0; at < usbchat::motor_pins.size(); ++at) {
        mask |= UINT64_C(1) << usbchat::motor_pins[at];
        rising_edges[at].store(0, std::memory_order_relaxed);
    }
    // Disabled pins may retain a prior pending status. It must not enter the new observation.
    gpio_ll_clear_intr_status(&GPIO, uint32_t(mask));
    gpio_ll_clear_intr_status_high(&GPIO, uint32_t(mask >> gpio_bank_width));
    unexpected_edges.store(0, std::memory_order_relaxed);
    observed_pin.store(selected, std::memory_order_relaxed);
    for (const auto pin : usbchat::motor_pins) {
        if (gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_POSEDGE) != ESP_OK ||
            gpio_intr_enable(static_cast<gpio_num_t>(pin)) != ESP_OK) {
            stop_observing();
            return false;
        }
    }
    return true;
}
bool edge_result() {
    bool ok = unexpected_edges.load(std::memory_order_relaxed) == 0;
    std::printf("EDGES");
    for (size_t at = 0; at < usbchat::motor_pins.size(); ++at) {
        const auto count = rising_edges[at].load(std::memory_order_relaxed);
        // A one-edge bookend tolerance is for pin identity, not precision pulse timing evidence.
        const bool expected = at == active_pin ? count + edge_tolerance >= requested_cycles &&
                                                     count <= requested_cycles + edge_tolerance
                                               : count == 0;
        ok = ok && expected;
        std::printf(" %lu=%lu", static_cast<unsigned long>(usbchat::motor_pins[at]),
                    static_cast<unsigned long>(count));
    }
    std::puts("");
    return ok;
}

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
        ok = ok && rc == ESP_OK && config.fun_sel == PIN_FUNC_GPIO && config.ie && config.oe &&
             expected && level == 0;
        std::printf(" %lu:%lu:%d", static_cast<unsigned long>(pin),
                    static_cast<unsigned long>(config.sig_out), level);
    }
    std::puts("");
    return ok;
}

bool coast() {
    bool ok = stop_observing();
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
    const bool observed = observed_pin.load(std::memory_order_relaxed) != no_active_pin;
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
    requested_cycles = duration;
    if (!start_observing(active_pin)) {
        fail("GPIO observer setup failed; no pulse sent");
        return;
    }
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
    if (gpio_install_isr_service(ESP_INTR_FLAG_IRAM) != ESP_OK) {
        fail("GPIO observer allocation failed");
    } else {
        for (size_t at = 0; at < usbchat::motor_pins.size(); ++at) {
            if (gpio_isr_handler_add(static_cast<gpio_num_t>(usbchat::motor_pins[at]), rising,
                                     reinterpret_cast<void *>(at)) != ESP_OK ||
                gpio_intr_disable(static_cast<gpio_num_t>(usbchat::motor_pins[at])) != ESP_OK) {
                fail("GPIO observer registration failed");
                break;
            }
        }
    }
    char line[line_capacity]{};
    size_t used = 0;
    bool discard = false;
    int64_t hello_at = 0;
    for (;;) {
        const auto now = esp_timer_get_time();
        if (!active && now - hello_at >= hello_interval_us) {
            std::puts(faulted ? "ERROR output unavailable; cut motor power; reset required"
                              : "AYNI MOTOR PROBE/4; D0..D3=38,39,40,41; CRC and pad-edge checks; "
                                "manual bench only");
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
            if (unexpected_edges.load(std::memory_order_relaxed)) {
                fail("unexpected edge on another GPIO");
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
