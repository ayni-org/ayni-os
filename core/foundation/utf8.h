// Unicode scalar validation shared by bounded payload owners; no normalization or I/O.
#ifndef AYNI_CORE_FOUNDATION_UTF8_H
#define AYNI_CORE_FOUNDATION_UTF8_H
#include <cstdint>
namespace ayni::foundation {
// Validate exactly size bytes, including U+0000; null is valid only for an empty range.
bool valid_utf8(const char *data, uint32_t size);
} // namespace ayni::foundation
#endif
