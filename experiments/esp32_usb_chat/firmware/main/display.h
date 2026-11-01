#pragma once
#include "protocol.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
namespace usbchat {
// I/O task owns the LCD; each DMA stripe remains unchanged until its completion.
class Display {
  public:
    bool initialize();
    bool paint(const uint8_t *rgb565, size_t bytes);

  private:
    static bool completed(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
    bool paint_stripe(size_t row);
    esp_lcd_panel_handle_t panel_ = nullptr;
    SemaphoreHandle_t done_ = nullptr;
    uint8_t *stripe_ = nullptr;
    bool faulted_ = false; // A timed-out DMA retains the stripe and forbids reuse.
};
} // namespace usbchat
