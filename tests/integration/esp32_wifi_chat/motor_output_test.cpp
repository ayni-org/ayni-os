// ADR-0112 / C-STOP-006: run the production MotorOutput against normal IDF lifecycle
// semantics. In particular, IDF 6.0.3 rmt_tx_destroy disables its GPIO output.
// These deterministic SDK substitutes do not measure physical timing or wheel motion.
#include "motor_output.h"
#include <algorithm>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <gtest/gtest.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>

struct TestCounter {
    int pin = -1, edges = 0;
};
struct TestTxChannel {
    int pin = -1;
    uint32_t remaining_ms = 0;
    bool enabled = false;
};
struct TestSyncManager {
    std::array<TestTxChannel *, 2> channels{};
};
namespace {
using namespace wifichat;
constexpr size_t gpio_count = 49;
constexpr uint32_t ticks_per_ms = 1000;
struct Pin {
    gpio_io_config_t config{};
    int level = 0;
};
std::array<Pin, gpio_count> pins{};
std::array<TestCounter *, motor_pins.size()> counters{};
std::array<TestTxChannel *, motor_reversed.size()> channels{};
TestSyncManager *synchronizer = nullptr;
unsigned created_channels = 0, deleted_channels = 0, encoders = 0;

// A finite 1 kHz waveform contributes one rising edge per elapsed millisecond.
// GPIO routing changes themselves produce no synthetic pulses in this SDK model.
void advance(uint32_t milliseconds) {
    for (auto *channel : channels) {
        if (!channel || !channel->enabled) {
            continue;
        }
        const auto emitted = std::min(milliseconds, channel->remaining_ms);
        for (auto *counter : counters) {
            if (counter && counter->pin == channel->pin) {
                counter->edges += int(emitted);
            }
        }
        channel->remaining_ms -= emitted;
    }
    esp_test_time_us.fetch_add(int64_t(milliseconds) * ticks_per_ms);
}
class MotorOutputTest : public testing::Test {
  protected:
    MotorOutput output;
    void SetUp() override {
        pins = {};
        created_channels = deleted_channels = encoders = 0;
        esp_test_time_us = 0;
        ASSERT_EQ(output.initialize(), ESP_OK);
    }
    void TearDown() override {
        EXPECT_EQ(output.close(), ESP_OK);
        EXPECT_EQ(synchronizer, nullptr);
        EXPECT_EQ(channels, (decltype(channels){}));
        EXPECT_EQ(counters, (decltype(counters){}));
        EXPECT_EQ(encoders, 0U);
        esp_test_time_us = -1;
    }
    void expect_coasting(size_t motor) {
        for (size_t direction = 0; direction < motor_inputs_per_channel; ++direction) {
            const auto pin = motor_pins[motor * motor_inputs_per_channel + direction];
            EXPECT_TRUE(pins[pin].config.oe) << pin;
            EXPECT_TRUE(pins[pin].config.ie) << pin;
            EXPECT_EQ(pins[pin].config.sig_out, SIG_GPIO_OUT_IDX) << pin;
            EXPECT_EQ(gpio_get_level(int(pin)), 0) << pin;
        }
    }
    void finish(uint32_t duration_ms) {
        advance(duration_ms);
        ASSERT_EQ(output.poll_segment(), MotorOutput::Poll::completed);
    }
};
} // namespace

esp_err_t gpio_config(const gpio_config_t *config) {
    for (size_t pin = 0; pin < pins.size(); ++pin) {
        if ((config->pin_bit_mask & (UINT64_C(1) << pin)) != 0) {
            pins[pin].config.fun_sel = PIN_FUNC_GPIO;
            pins[pin].config.ie = pins[pin].config.oe = config->mode == GPIO_MODE_INPUT_OUTPUT;
        }
    }
    return ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level) {
    pins[size_t(pin)].level = int(level);
    return ESP_OK;
}
int gpio_get_level(gpio_num_t pin) {
    return pins[size_t(pin)].level;
}
esp_err_t gpio_get_io_config(gpio_num_t pin, gpio_io_config_t *config) {
    *config = pins[size_t(pin)].config;
    return ESP_OK;
}
void esp_rom_gpio_connect_out_signal(uint32_t pin, uint32_t signal, bool, bool) {
    pins[pin].config.sig_out = int(signal);
}
esp_err_t pcnt_new_unit(const pcnt_unit_config_t *, pcnt_unit_handle_t *unit) {
    for (auto &counter : counters) {
        if (!counter) {
            counter = new TestCounter;
            *unit = counter;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}
esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit, const pcnt_chan_config_t *config,
                           pcnt_channel_handle_t *channel) {
    unit->pin = config->edge_gpio_num;
    *channel = unit;
    return ESP_OK;
}
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t, int, int) {
    return ESP_OK;
}
esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t, int, int) {
    return ESP_OK;
}
esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t, const void *) {
    return ESP_OK;
}
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t, int) {
    return ESP_OK;
}
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t) {
    return ESP_OK;
}
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit) {
    unit->edges = 0;
    return ESP_OK;
}
esp_err_t pcnt_unit_start(pcnt_unit_handle_t) {
    return ESP_OK;
}
esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t unit, int *count) {
    *count = unit->edges;
    return ESP_OK;
}
esp_err_t pcnt_unit_stop(pcnt_unit_handle_t) {
    return ESP_OK;
}
esp_err_t pcnt_unit_disable(pcnt_unit_handle_t) {
    return ESP_OK;
}
esp_err_t pcnt_del_channel(pcnt_channel_handle_t) {
    return ESP_OK;
}
esp_err_t pcnt_del_unit(pcnt_unit_handle_t unit) {
    for (auto &counter : counters) {
        if (counter == unit) {
            counter = nullptr;
        }
    }
    delete unit;
    return ESP_OK;
}
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *, rmt_encoder_handle_t *encoder) {
    *encoder = new TestEncoder;
    ++encoders;
    return ESP_OK;
}
esp_err_t rmt_del_encoder(rmt_encoder_handle_t encoder) {
    delete encoder;
    --encoders;
    return ESP_OK;
}
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *config, rmt_channel_handle_t *out) {
    for (auto &channel : channels) {
        if (channel) {
            continue;
        }
        // The opposite H-bridge input must be actively low before connecting a new waveform.
        const auto found =
            std::find(motor_pins.begin(), motor_pins.end(), uint32_t(config->gpio_num));
        if (found == motor_pins.end()) {
            return ESP_ERR_INVALID_ARG;
        }
        const auto input = size_t(found - motor_pins.begin());
        const auto opposite = motor_pins[input ^ 1U];
        EXPECT_TRUE(pins[opposite].config.oe);
        EXPECT_EQ(pins[opposite].config.sig_out, SIG_GPIO_OUT_IDX);
        EXPECT_EQ(pins[opposite].level, 0);
        channel = new TestTxChannel{config->gpio_num};
        auto &pin = pins[size_t(config->gpio_num)];
        pin.config.oe = true;
        pin.config.sig_out = config->gpio_num; // A distinct, non-GPIO peripheral route.
        *out = channel;
        ++created_channels;
        return ESP_OK;
    }
    return ESP_FAIL;
}
esp_err_t rmt_enable(rmt_channel_handle_t channel) {
    if (channel->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    channel->enabled = true;
    return ESP_OK;
}
esp_err_t rmt_disable(rmt_channel_handle_t channel) {
    if (!channel->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    channel->enabled = false;
    channel->remaining_ms = 0;
    return ESP_OK;
}
esp_err_t rmt_del_channel(rmt_channel_handle_t channel) {
    if (channel->enabled || synchronizer) {
        return ESP_ERR_INVALID_STATE;
    }
    // Match components/esp_driver_rmt/src/rmt_tx.c:rmt_tx_destroy in pinned IDF 6.0.3.
    pins[size_t(channel->pin)].config.oe = false;
    for (auto &slot : channels) {
        if (slot == channel) {
            slot = nullptr;
        }
    }
    delete channel;
    ++deleted_channels;
    return ESP_OK;
}
esp_err_t rmt_new_sync_manager(const rmt_sync_manager_config_t *config,
                               rmt_sync_manager_handle_t *out) {
    if (synchronizer || config->array_size != channels.size()) {
        return ESP_ERR_INVALID_STATE;
    }
    synchronizer = new TestSyncManager;
    std::copy_n(config->tx_channel_array, channels.size(), synchronizer->channels.begin());
    *out = synchronizer;
    return rmt_sync_reset(synchronizer);
}
esp_err_t rmt_del_sync_manager(rmt_sync_manager_handle_t sync) {
    EXPECT_EQ(sync, synchronizer);
    delete sync;
    synchronizer = nullptr;
    return ESP_OK;
}
esp_err_t rmt_sync_reset(rmt_sync_manager_handle_t sync) {
    for (const auto *channel : sync->channels) {
        if (!channel->enabled || channel->remaining_ms) {
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}
esp_err_t rmt_transmit(rmt_channel_handle_t channel, rmt_encoder_handle_t, const void *payload,
                       size_t bytes, const rmt_transmit_config_t *config) {
    if (!channel->enabled || channel->remaining_ms) {
        return ESP_ERR_INVALID_STATE;
    }
    const auto *symbols = static_cast<const rmt_symbol_word_t *>(payload);
    uint32_t ticks = 0;
    for (size_t at = 0; at < bytes / sizeof(*symbols); ++at) {
        ticks += symbols[at].duration0 + symbols[at].duration1;
    }
    channel->remaining_ms = ticks / ticks_per_ms * uint32_t(config->loop_count);
    return ESP_OK;
}
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t channel, int) {
    return channel->remaining_ms ? ESP_ERR_TIMEOUT : ESP_OK;
}

// ADR-0112: the fifth all-zero segment is timed, completed and never adds PWM edges.
TEST_F(MotorOutputTest, five_segments_finish_with_final_coast) {
    constexpr uint32_t segment_ms = 400;
    const std::array<MotorCommand, 5> trajectory{{{50, -50, segment_ms},
                                                  {-50, 50, segment_ms},
                                                  {50, -50, segment_ms},
                                                  {-50, 50, segment_ms},
                                                  {0, 0, segment_ms}}};
    ASSERT_EQ(output.start(trajectory.front()), ESP_OK);
    for (size_t at = 0; at < trajectory.size(); ++at) {
        if (at) {
            ASSERT_EQ(output.transition(trajectory[at]), ESP_OK) << "segment " << at + 1;
        }
        EXPECT_EQ(output.poll_segment(), MotorOutput::Poll::running);
        if (at == trajectory.size() - 1) {
            EXPECT_TRUE(output.disabled());
            expect_coasting(0);
            expect_coasting(1);
        }
        finish(segment_ms);
    }
    std::array<int, 4> edges{};
    ASSERT_TRUE(output.counts(edges));
    EXPECT_EQ(edges, (std::array<int, 4>{800, 800, 800, 800}));
    EXPECT_EQ(output.last_error(), ESP_OK);
}

// ADR-0112: one coasting motor keeps both inputs low while the other continues its waveform.
TEST_F(MotorOutputTest, single_motor_coasts_and_then_rejoins) {
    constexpr uint32_t segment_ms = 100;
    ASSERT_EQ(output.start({50, 50, segment_ms}), ESP_OK);
    finish(segment_ms);
    ASSERT_EQ(output.transition({0, 50, segment_ms}), ESP_OK);
    expect_coasting(0);
    finish(segment_ms);
    ASSERT_EQ(output.transition({50, 50, segment_ms}), ESP_OK);
    finish(segment_ms);
    EXPECT_EQ(output.expected_counts(), (std::array<uint32_t, 4>{200, 0, 0, 300}));
}

// ADR-0114: a hot transition to coast preserves the measured partial-output baseline.
TEST_F(MotorOutputTest, hot_coast_finishes_and_allows_a_later_moving_segment) {
    ASSERT_EQ(output.start({50, 50, 1000}), ESP_OK);
    advance(100);
    ASSERT_EQ(output.replace_active({0, 0, 200}), ESP_OK);
    EXPECT_TRUE(output.disabled());
    EXPECT_EQ(output.poll_segment(), MotorOutput::Poll::running);
    finish(200);
    ASSERT_EQ(output.transition({-50, -50, 100}), ESP_OK);
    finish(100);
    std::array<int, 4> edges{};
    ASSERT_TRUE(output.counts(edges));
    EXPECT_EQ(edges, (std::array<int, 4>{100, 100, 100, 100}));
}

// ADR-0112: reversal leaves the former output actively low before attaching opposite polarity.
TEST_F(MotorOutputTest, reversal_keeps_each_opposite_input_driven_low) {
    ASSERT_EQ(output.start({50, 50, 100}), ESP_OK);
    finish(100);
    ASSERT_EQ(output.transition({-50, -50, 100}), ESP_OK);
    finish(100);
    ASSERT_EQ(output.transition({50, 50, 100}), ESP_OK);
    finish(100);
}

// ADR-0112: same-polarity segment changes still reuse both channels and the sync manager.
TEST_F(MotorOutputTest, same_direction_segments_reuse_channels) {
    ASSERT_EQ(output.start({30, 40, 100}), ESP_OK);
    finish(100);
    ASSERT_EQ(output.transition({60, 70, 100}), ESP_OK);
    finish(100);
    EXPECT_EQ(created_channels, 2U);
    EXPECT_EQ(deleted_channels, 0U);
    ASSERT_EQ(output.replace_active({40, 50, 100}), ESP_OK);
    finish(100);
    EXPECT_EQ(created_channels, 2U);
}

// C-STOP-006: explicit stop reclaims low GPIO and permits a separate, newly started waveform.
TEST_F(MotorOutputTest, explicit_stop_reclaims_gpio_before_next_start) {
    ASSERT_EQ(output.start({50, 50, 1000}), ESP_OK);
    advance(100);
    ASSERT_EQ(output.stop(), ESP_OK);
    EXPECT_TRUE(output.disabled());
    ASSERT_EQ(output.start({-50, -50, 100}), ESP_OK);
    finish(100);
}

// ADR-0112: a normal trajectory may begin coasting before its first moving segment.
TEST_F(MotorOutputTest, initial_coast_transitions_to_motion_and_back) {
    ASSERT_EQ(output.start({0, 0, 100}), ESP_OK);
    EXPECT_TRUE(output.disabled());
    finish(100);
    ASSERT_EQ(output.transition({50, 50, 100}), ESP_OK);
    finish(100);
    ASSERT_EQ(output.transition({0, 0, 100}), ESP_OK);
    finish(100);
}
