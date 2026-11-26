#pragma once
#include <cstdint>
void esp_rom_gpio_connect_out_signal(uint32_t pin, uint32_t signal, bool invert,
                                     bool invert_enable);
