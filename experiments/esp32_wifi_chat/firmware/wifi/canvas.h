#pragma once
#include "../main/protocol.h"
#include <array>
#include <miniz.h>
#include <string_view>

namespace wifichat::wifi {
// UI-owned framebuffer and immutable ROM glyphs. API/remote workers never draw.
class Canvas {
  public:
    static constexpr uint16_t background = 0x0863, foreground = 0xef7e, accent = 0x4e7b;
    static constexpr uint16_t surface = 0x10e6, muted = 0x8c93, positive = 0x5e95, danger = 0xfb6d;
    void clear();
    void rectangle(int x, int y, int width, int height, uint16_t color);
    void line(int x0, int y0, int x1, int y1, uint16_t color);
    void rounded_rectangle(int x, int y, int width, int height, int radius, uint16_t color);
    void label(int x, int y, std::string_view text, uint16_t color = foreground,
               int max_width = screen_width);
    void button(int x, int y, int width, int height, std::string_view label,
                uint16_t fill = surface, uint16_t ink = foreground);
    void heading(std::string_view title);
    void setting_row(int top, int height, std::string_view title, std::string_view value);
    void toggle(int x, int y, bool enabled);
    // Paginate independently of the visible page; returns at least one page.
    size_t document(std::string_view text, size_t page, int top, int bottom);
    const uint8_t *pixels() const { return pixels_.data(); }

  private:
    void glyph(int x, int y, uint32_t code, uint16_t color);
    const uint8_t *cell(uint32_t code);
    int text_width(std::string_view text);
    static constexpr size_t block_cells = 64, cell_bytes = 33;
    std::array<uint8_t, block_cells * cell_bytes> glyph_block_{};
    // ROM's convenience function puts this multi-KiB state on the calling
    // stack. Keep it with the framebuffer in PSRAM instead (UI task only).
    tinfl_decompressor decompressor_{};
    size_t cached_block_ = SIZE_MAX;
    std::array<uint8_t, screen_bytes> pixels_{};
};
} // namespace wifichat::wifi
