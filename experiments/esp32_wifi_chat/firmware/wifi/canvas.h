#pragma once
#include "../main/protocol.h"
#include <array>
#include <miniz.h>
#include <string_view>

namespace wifichat::wifi {
// UI-owned framebuffer and immutable ROM glyphs. API/remote workers never draw.
class Canvas {
  public:
    static constexpr uint16_t background = 0x0841, foreground = 0xffff, accent = 0x2c9f;
    void clear();
    void rectangle(int x, int y, int width, int height, uint16_t color);
    void label(int x, int y, std::string_view text, uint16_t color = foreground);
    void button(int x, int y, int width, int height, std::string_view label);
    // Paginate independently of the visible page; returns at least one page.
    size_t document(std::string_view text, size_t page, int top, int bottom);
    const uint8_t *pixels() const { return pixels_.data(); }

  private:
    void glyph(int x, int y, uint32_t code, uint16_t color);
    const uint8_t *cell(uint32_t code);
    static constexpr size_t block_cells = 64, cell_bytes = 33;
    std::array<uint8_t, block_cells * cell_bytes> glyph_block_{};
    // ROM's convenience function puts this multi-KiB state on the calling
    // stack. Keep it with the framebuffer in PSRAM instead (UI task only).
    tinfl_decompressor decompressor_{};
    size_t cached_block_ = SIZE_MAX;
    std::array<uint8_t, screen_bytes> pixels_{};
};
} // namespace wifichat::wifi
