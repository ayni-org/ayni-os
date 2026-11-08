#include "canvas.h"
#include <algorithm>
#include <cstring>
#include <miniz.h>
extern const uint8_t font_start[] asm("_binary_ayni_font_bin_start");
namespace wifichat::wifi {
namespace {
uint32_t next(std::string_view text, size_t &at) {
    const auto first = uint8_t(text[at++]);
    if (first < 0x80) {
        return first;
    }
    const size_t count = first >= 0xf0 && first <= 0xf4  ? 3
                         : first >= 0xe0 && first < 0xf0 ? 2
                         : first >= 0xc2 && first < 0xe0 ? 1
                                                         : 0;
    if (!count || text.size() - at < count) {
        return '?';
    }
    uint32_t code = first & ((1U << (6 - count)) - 1);
    for (size_t i = 0; i < count; ++i) {
        if ((uint8_t(text[at]) & 0xc0) != 0x80) {
            return '?';
        }
        code = (code << 6) | (uint8_t(text[at++]) & 0x3f);
    }
    const uint32_t minimum[] = {0, 0x80, 0x800, 0x10000};
    return code < minimum[count] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) ? '?'
                                                                                          : code;
}
} // namespace
const uint8_t *Canvas::cell(uint32_t code) {
    constexpr size_t header_bytes = 16, range_bytes = 12, block_bytes = 8;
    constexpr size_t range_count_offset = 6, glyph_count_offset = 8;
    const auto count =
        size_t(font_start[range_count_offset]) | (size_t(font_start[range_count_offset + 1]) << 8U);
    size_t glyph = '?';
    for (size_t i = 0; i < count; ++i) {
        const auto *range = font_start + header_bytes + i * range_bytes;
        const auto start = get32(range), end = get32(range + 4);
        if (code >= start && code < end) {
            glyph = get32(range + 8) + code - start;
            break;
        }
    }
    const auto block = glyph / block_cells;
    if (cached_block_ != block) {
        const auto *entry = font_start + header_bytes + count * range_bytes + block * block_bytes;
        const auto size = get32(entry + 4), offset = get32(entry);
        const auto expected = std::min(block_cells, size_t(get32(font_start + glyph_count_offset)) -
                                                        block * block_cells) *
                              cell_bytes;
        glyph_block_.fill(0);
        tinfl_init(&decompressor_);
        size_t input_bytes = size, output_bytes = glyph_block_.size();
        const auto status = tinfl_decompress(
            &decompressor_, font_start + offset, &input_bytes, glyph_block_.data(),
            glyph_block_.data(), &output_bytes,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (status != TINFL_STATUS_DONE || output_bytes != expected || input_bytes != size) {
            static constexpr uint8_t missing[cell_bytes] = {8};
            return missing;
        }
        cached_block_ = block;
    }
    return glyph_block_.data() + (glyph % block_cells) * cell_bytes;
}
void Canvas::clear() {
    rectangle(0, 0, screen_width, screen_height, background);
}
void Canvas::rectangle(int x, int y, int width, int height, uint16_t color) {
    for (int row = std::max(0, y); row < std::min(int(screen_height), y + height); ++row) {
        for (int col = std::max(0, x); col < std::min(int(screen_width), x + width); ++col) {
            const auto at = size_t(row * screen_width + col) * sizeof(uint16_t);
            pixels_[at] = uint8_t(color >> 8);
            pixels_[at + 1] = uint8_t(color);
        }
    }
}
void Canvas::glyph(int x, int y, uint32_t code, uint16_t color) {
    const auto *bits = cell(code) + 1;
    for (int row = 0; row < 16; ++row) {
        for (int col = 0; col < 16; ++col) {
            if (bits[row * 2 + col / 8] & (0x80U >> (col % 8))) {
                rectangle(x + col, y + row, 1, 1, color);
            }
        }
    }
}
void Canvas::label(int x, int y, std::string_view text, uint16_t color) {
    for (size_t at = 0; at < text.size();) {
        const auto code = next(text, at);
        const auto width = cell(code)[0];
        if (x + width > int(screen_width)) {
            break;
        }
        glyph(x, y, code, color);
        x += width;
    }
}
void Canvas::button(int x, int y, int width, int height, std::string_view text) {
    rectangle(x, y, width, height, accent);
    label(x + 4, y + (height - 16) / 2, text);
}
size_t Canvas::document(std::string_view text, size_t page, int top, int bottom) {
    int x = 8, y = top;
    size_t current = 0;
    constexpr int line_height = 20, margin = 8;
    for (size_t at = 0; at < text.size();) {
        const auto code = next(text, at);
        const auto width = cell(code)[0];
        if (code == '\r') {
            continue;
        }
        if (code == '\n' || x + width > int(screen_width) - margin) {
            x = margin;
            y += line_height;
        }
        if (y + 16 > bottom) {
            y = top;
            ++current;
        }
        if (code != '\n') {
            if (current == page) {
                glyph(x, y, code, foreground);
            }
            x += width;
        }
    }
    return current + 1;
}
} // namespace wifichat::wifi
