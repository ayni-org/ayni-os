#pragma once
#include <cstdint>
#include <esp_err.h>
using gpio_num_t = int;
inline constexpr int GPIO_MODE_INPUT_OUTPUT = 3, GPIO_PULLDOWN_ENABLE = 1;
struct gpio_config_t {
    uint64_t pin_bit_mask = 0;
    int mode = 0, pull_down_en = 0;
};
struct gpio_io_config_t {
    int fun_sel = 0, sig_out = 0;
    bool ie = false, oe = false;
};
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_get_io_config(gpio_num_t pin, gpio_io_config_t *config);
