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
    frames_lock_ = xSemaphoreCreateMutex();
    worker_tcb_ = static_cast<StaticTask_t *>(
        heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    worker_stack_ = static_cast<StackType_t *>(
        heap_caps_malloc(stack_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    for (auto &frame : frames_) {
        frame.pixels = static_cast<uint8_t *>(
            heap_caps_malloc(screen_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    stripe_ = static_cast<uint8_t *>(
        heap_caps_malloc(stripe_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    shadow_ =
        static_cast<uint8_t *>(heap_caps_malloc(screen_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!done_ || !frames_lock_ || !worker_tcb_ || !worker_stack_ || !stripe_ || !shadow_ ||
        !frames_[0].pixels || !frames_[1].pixels) {
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
    if (esp_lcd_panel_disp_on_off(panel_, true) != ESP_OK) {
        return false;
    }
    worker_ = xTaskCreateStatic(worker_entry, "lcd-display", stack_bytes, this, priority,
                                worker_stack_, worker_tcb_);
    return worker_ != nullptr;
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
bool Display::submit_ui_frame(const uint8_t *pixels, size_t bytes) {
    return submit(0, 0, pixels, bytes, nullptr, nullptr);
}
bool Display::submit_tool_frame(uint64_t call, uint32_t pages, const uint8_t *pixels, size_t bytes,
                                void *context, Completion completion) {
    return call && completion && submit(call, pages, pixels, bytes, context, completion);
}
// The short metadata/copy lock never covers SPI or callbacks. Producers fail immediately on
// contention; ordinary UI retries/latest-wins, and reliable tool callers retain their request.
bool Display::submit(uint64_t call, uint32_t pages, const uint8_t *pixels, size_t bytes,
                     void *context, Completion completion) {
    if (!worker_ || !pixels || bytes != screen_bytes || xSemaphoreTake(frames_lock_, 0) != pdTRUE) {
        return false;
    }
    if (pending_ && frames_[*pending_].call) {
        xSemaphoreGive(frames_lock_);
        return false; // A pending authorized tool is never overwritten by an ordinary UI frame.
    }
    const size_t index = pending_ ? *pending_ : active_ ? 1U - *active_ : 0;
    auto &frame = frames_[index];
    std::memcpy(frame.pixels, pixels, bytes);
    frame.call = call;
    frame.pages = pages;
    frame.context = context;
    frame.completion = completion;
    pending_ = index;
    xSemaphoreGive(frames_lock_);
    xTaskNotifyGive(worker_);
    return true;
}
void Display::worker_entry(void *context) {
    static_cast<Display *>(context)->run();
}
void Display::run() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            xSemaphoreTake(frames_lock_, portMAX_DELAY);
            if (!pending_) {
                xSemaphoreGive(frames_lock_);
                break;
            }
            active_ = pending_;
            pending_.reset();
            const auto frame = frames_[*active_];
            xSemaphoreGive(frames_lock_);
            const bool painted = render(frame.pixels, screen_bytes);
            uint8_t stage = 0;
            while (frame.completion &&
                   !frame.completion(frame.context, frame.call, frame.pages, painted, stage)) {
                vTaskDelay(1); // Retain exact completion under input backpressure; no second DMA.
            }
            xSemaphoreTake(frames_lock_, portMAX_DELAY);
            active_.reset();
            xSemaphoreGive(frames_lock_);
        }
    }
}
bool Display::render(const uint8_t *pixels, size_t bytes) {
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
