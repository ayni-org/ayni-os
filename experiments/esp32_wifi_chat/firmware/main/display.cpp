#include "display.h"
#include <cstdio>
#include <cstring>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
namespace wifichat {
namespace {
constexpr int lcd_sck = 12, lcd_sdi = 11, lcd_cs = 10, lcd_dc = 9, lcd_reset = 8;
constexpr size_t stripe_rows = 8, stripe_bytes = screen_width * stripe_rows * sizeof(uint16_t);
// Keep the established 40 MHz bus. Delta updates keep unchanged navigation
// buttons off the SPI bus while model/voice status changes.
constexpr int lcd_hz = 40000000;
} // namespace
bool Display::completed(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *ctx) {
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(static_cast<Display *>(ctx)->done_, &wake);
    return wake == pdTRUE;
}
bool Display::initialize() {
    done_ = xSemaphoreCreateBinary();
    stripe_ = static_cast<uint8_t *>(
        heap_caps_malloc(stripe_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    shadow_ =
        static_cast<uint8_t *>(heap_caps_malloc(screen_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!done_ || !stripe_ || !shadow_) {
        return false;
    }
    spi_bus_config_t bus{};
    bus.mosi_io_num = lcd_sdi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = lcd_sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = stripe_bytes;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        return false;
    }
    esp_lcd_panel_io_spi_config_t io{};
    io.cs_gpio_num = static_cast<gpio_num_t>(lcd_cs);
    io.dc_gpio_num = static_cast<gpio_num_t>(lcd_dc);
    io.spi_mode = 0;
    io.pclk_hz = lcd_hz;
    io.trans_queue_depth = 1;
    io.on_color_trans_done = completed;
    io.user_ctx = this;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    esp_lcd_panel_io_handle_t port = nullptr;
    if (esp_lcd_new_panel_io_spi(SPI2_HOST, &io, &port) != ESP_OK) {
        return false;
    }
    esp_lcd_panel_dev_config_t config{};
    config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
    config.bits_per_pixel = 16;
    config.reset_gpio_num = static_cast<gpio_num_t>(lcd_reset);
    if (esp_lcd_new_panel_st7789(port, &config, &panel_) != ESP_OK ||
        esp_lcd_panel_reset(panel_) != ESP_OK || esp_lcd_panel_init(panel_) != ESP_OK ||
        esp_lcd_panel_invert_color(panel_, true) != ESP_OK ||
        esp_lcd_panel_disp_on_off(panel_, false) != ESP_OK) {
        return false;
    }
    // Reset does not initialize display RAM. Clear it with the existing DMA lane
    // before enabling scanout, so waiting for the first tool call shows black.
    std::memset(stripe_, 0, stripe_bytes);
    std::memset(shadow_, 0, screen_bytes);
    for (size_t row = 0; row < screen_height; row += stripe_rows) {
        if (!paint_stripe(row)) {
            return false;
        }
    }
    return esp_lcd_panel_disp_on_off(panel_, true) == ESP_OK;
}
bool Display::paint_stripe(size_t row) {
    if (esp_lcd_panel_draw_bitmap(panel_, 0, row, screen_width, row + stripe_rows, stripe_) !=
            ESP_OK ||
        xSemaphoreTake(done_, pdMS_TO_TICKS(1000)) != pdTRUE) {
        faulted_ = true;
        return false;
    }
    return true;
}
bool Display::paint(const uint8_t *pixels, size_t bytes) {
    if (!panel_ || !shadow_ || !pixels || faulted_ || bytes != screen_bytes) {
        return false;
    }
    size_t changed = 0, navigation = 0;
    constexpr size_t navigation_top = 248;
    for (size_t row = 0; row < screen_height; row += stripe_rows) {
        const auto offset = row * screen_width * sizeof(uint16_t);
        if (!std::memcmp(shadow_ + offset, pixels + offset, stripe_bytes)) {
            continue;
        }
        std::memcpy(stripe_, pixels + offset, stripe_bytes);
        if (!paint_stripe(row)) {
            return false;
        }
        // Only an acknowledged DMA may update the shadow. Failed/timed-out
        // transfers keep the existing fault fence and never invent a painted frame.
        std::memcpy(shadow_ + offset, pixels + offset, stripe_bytes);
        ++changed;
        if (row >= navigation_top) {
            ++navigation;
        }
    }
    if (changed) {
        std::printf("wifi lcd changed_stripes=%zu navigation_stripes=%zu\n", changed, navigation);
    }
    return true;
}
} // namespace wifichat
