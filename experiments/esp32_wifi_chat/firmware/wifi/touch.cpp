#include "touch.h"
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
namespace wifichat::wifi {
bool Touch::initialize() {
    constexpr gpio_num_t reset = GPIO_NUM_21;
    gpio_config_t pin{};
    pin.pin_bit_mask = UINT64_C(1) << reset;
    pin.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&pin) != ESP_OK || gpio_set_level(reset, 0) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(reset, 1);
    vTaskDelay(pdMS_TO_TICKS(200));
    i2c_master_bus_config_t config{};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = GPIO_NUM_17;
    config.scl_io_num = GPIO_NUM_18;
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&config, &bus_) != ESP_OK) {
        return false;
    }
    i2c_device_config_t device{};
    device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device.device_address = 0x38;
    device.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(bus_, &device, &device_) != ESP_OK) {
        return false;
    }
    uint8_t address = 0xa8, vendor = 0;
    return i2c_master_transmit_receive(device_, &address, 1, &vendor, 1, 50) == ESP_OK &&
           vendor != 0 && vendor != 0xff;
}
bool Touch::released(int &x, int &y) {
    if (!device_) {
        return false;
    }
    uint8_t address = 2, data[5]{};
    if (i2c_master_transmit_receive(device_, &address, 1, data, sizeof(data), 20) != ESP_OK) {
        down_ = false; // An invalid sample never manufactures a tap.
        return false;
    }
    if ((data[0] & 0x0f) == 1) {
        x_ = ((data[1] & 0x0f) << 8) | data[2];
        y_ = ((data[3] & 0x0f) << 8) | data[4];
        down_ = x_ < 240 && y_ < 320;
        return false;
    }
    const bool released = down_ && !(data[0] & 0x0f);
    down_ = false;
    x = x_;
    y = y_;
    return released;
}
} // namespace wifichat::wifi
