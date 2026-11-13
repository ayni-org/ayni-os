#pragma once
// Native render-only adapter: ROM glyph blocks use ordinary zlib streams.
#include <cstddef>
#include <zlib.h>
struct tinfl_decompressor {};
constexpr int TINFL_STATUS_DONE = 0, TINFL_FLAG_PARSE_ZLIB_HEADER = 1,
              TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 2;
inline void tinfl_init(tinfl_decompressor *) {}
inline int tinfl_decompress(tinfl_decompressor *, const void *input, size_t *input_bytes, void *,
                            void *output, size_t *output_bytes, int) {
    uLongf count = *output_bytes;
    const auto result = uncompress(static_cast<Bytef *>(output), &count,
                                   static_cast<const Bytef *>(input), *input_bytes);
    *output_bytes = count;
    return result == Z_OK ? TINFL_STATUS_DONE : -1;
}
