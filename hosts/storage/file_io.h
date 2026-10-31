// Exact bounded file reads shared by Host mechanisms; adapters may return short reads.
#ifndef AYNI_HOST_FILE_IO_H
#define AYNI_HOST_FILE_IO_H
#include <ayni/hosts/storage/files.h>
#include <cerrno>
namespace ayni::hosts::storage::io {
// Positive short reads are normal file-port behavior, not an end-of-file proof.
inline int read_up_to(const Files &files, int handle, uint64_t at, uint8_t *out, std::size_t size,
                      std::size_t &total) {
    total = 0;
    while (total < size) {
        std::size_t count = 0;
        const auto error = files.read(handle, at + total, out + total, size - total, count);
        if (error != 0) {
            return error;
        }
        if (count > size - total) {
            return EIO;
        }
        if (count == 0) {
            break;
        }
        total += count;
    }
    return 0;
}

// A checked frame promises all requested bytes; an early EOF is an I/O error.
inline int read_exact(const Files &files, int handle, uint64_t at, uint8_t *out, std::size_t size) {
    std::size_t count = 0;
    const auto error = read_up_to(files, handle, at, out, size, count);
    return error != 0 ? error : count == size ? 0 : EIO;
}

} // namespace ayni::hosts::storage::io
#endif
