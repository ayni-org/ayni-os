#pragma once
#include "protocol.h"
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <optional>
namespace wifichat {
// The low-priority display task alone owns SPI/DMA and the acknowledged shadow.
// UI producers copy into an idle PSRAM slot without waiting for DMA.
class Display {
  public:
    bool initialize();
    using Completion = bool (*)(void *, uint64_t call, uint32_t pages, bool painted,
                                uint8_t &stage);
    bool submit_ui_frame(const uint8_t *rgb565, size_t bytes);
    // A tool frame is reliable and cannot be superseded. Callback runs on lcd-display
    // after all DMA stripes finish; false retains/retries completion without re-rendering.
    bool submit_tool_frame(uint64_t call, uint32_t pages, const uint8_t *, size_t, void *context,
                           Completion);
    bool paint(const uint8_t *pixels, size_t bytes) { return submit_ui_frame(pixels, bytes); }

  private:
    static bool completed(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *);
    bool paint_stripe(size_t row);
    bool render(const uint8_t *, size_t);
    bool submit(uint64_t call, uint32_t pages, const uint8_t *, size_t, void *, Completion);
    static void worker_entry(void *);
    void run();
    struct Frame {
        uint8_t *pixels = nullptr; // PSRAM; exclusively producer, pending, or DMA-worker owned.
        uint64_t call = 0;         // 0 = coalescible UI; nonzero = reliable tool.
        uint32_t pages = 0;
        void *context = nullptr;
        Completion completion = nullptr;
    };
    static constexpr uint32_t stack_bytes = 4096, priority = 2;
    static constexpr size_t frame_slots = 2;
    std::array<Frame, frame_slots> frames_{};
    SemaphoreHandle_t frames_lock_ = nullptr;
    std::optional<size_t> active_, pending_,
        writing_; // Guarded by frames_lock_; disjoint if both present.
    TaskHandle_t worker_ = nullptr;
    StaticTask_t *worker_tcb_ = nullptr;  // Internal RAM: IDF rejects PSRAM task control blocks.
    StackType_t *worker_stack_ = nullptr; // Internal RAM, boot lifetime like panel/IO callback.
    esp_lcd_panel_handle_t panel_ = nullptr;
    SemaphoreHandle_t done_ = nullptr;
    uint8_t *stripe_ = nullptr;
    uint8_t *shadow_ = nullptr; // PSRAM copy of acknowledged LCD pixels, display-worker only.
    bool faulted_ = false;      // A timed-out DMA retains the stripe and forbids reuse.
};
} // namespace wifichat
