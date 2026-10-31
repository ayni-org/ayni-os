#include "posix_files.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace ayni::hosts::native {
int PosixFiles::begin(const char *name) {
    if (directory_ != invalid_handle) {
        return EBUSY;
    }
    directory_ = ::open(name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_ < 0) {
        return errno;
    }
    lock_ = ::openat(directory_, ".ayni_storage_lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_ < 0 || ::flock(lock_, LOCK_EX | LOCK_NB) != 0) {
        const auto error = errno;
        end();
        return error;
    }
    return 0;
}
void PosixFiles::end() {
    if (lock_ != invalid_handle) {
        ::close(lock_);
    }
    if (directory_ != invalid_handle) {
        ::close(directory_);
    }
    lock_ = directory_ = invalid_handle;
}
int PosixFiles::open(const char *name, Mode mode, int &handle) {
    int flags = O_CLOEXEC;
    switch (mode) {
    case Mode::read_existing:
        flags |= O_RDONLY;
        break;
    case Mode::read_existing_no_follow:
        flags |= O_RDONLY | O_NOFOLLOW;
        break;
    case Mode::read_write_create:
        flags |= O_RDWR | O_CREAT;
        break;
    case Mode::replace:
        flags |= O_RDWR | O_CREAT | O_TRUNC;
        break;
    default:
        return EINVAL;
    }
    const int next = ::openat(directory_, name, flags, 0600);
    if (next < 0) {
        return errno;
    }
    handle = next;
    return 0;
}
void PosixFiles::close(int handle) {
    ::close(handle);
}
int PosixFiles::lock(int handle) {
    return ::flock(handle, LOCK_EX | LOCK_NB) == 0 ? 0 : errno;
}
int PosixFiles::read(int handle, uint64_t offset, uint8_t *data, std::size_t bytes,
                     std::size_t &count) const {
    count = 0;
    ssize_t got = 0;
    do {
        got = ::pread(handle, data, bytes, static_cast<off_t>(offset));
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        return errno;
    }
    count = static_cast<std::size_t>(got);
    return 0;
}
int PosixFiles::write(int handle, uint64_t offset, const uint8_t *data, std::size_t bytes) {
    if (io_.write) {
        return io_.write(io_.context, handle, offset, data, bytes);
    }
    while (bytes > 0) {
        const auto put = ::pwrite(handle, data, bytes, static_cast<off_t>(offset));
        if (put < 0 && errno == EINTR) {
            continue;
        }
        if (put < 0) {
            return errno;
        }
        if (put == 0) {
            return EIO;
        }
        const auto count = static_cast<std::size_t>(put);
        data += count;
        offset += count;
        bytes -= count;
    }
    return 0;
}
int PosixFiles::size(int handle, uint64_t &bytes) const {
    struct stat status {};
    if (::fstat(handle, &status) != 0) {
        return errno;
    }
    bytes = static_cast<uint64_t>(status.st_size);
    return 0;
}
int PosixFiles::truncate(int handle, uint64_t bytes) {
    return ::ftruncate(handle, static_cast<off_t>(bytes)) == 0 ? 0 : errno;
}
// C-HOST-010: confirm data and length. The macOS branch retains the existing
// stronger drive-cache request; it remains outside the validated Native platforms.
int PosixFiles::sync_file(int handle) {
    if (io_.sync_file) {
        return io_.sync_file(io_.context, handle);
    }
#if defined(__APPLE__)
    return ::fcntl(handle, F_FULLFSYNC) == 0 ? 0 : errno;
#else
    while (::fdatasync(handle) != 0) {
        if (errno != EINTR) {
            return errno;
        }
    }
    return 0;
#endif
}
int PosixFiles::sync_namespace() {
    return io_.sync_directory ? io_.sync_directory(io_.context, directory_)
                              : (::fsync(directory_) == 0 ? 0 : errno);
}
int PosixFiles::rename(const char *from, const char *to) {
    return ::renameat(directory_, from, directory_, to) == 0 ? 0 : errno;
}
int PosixFiles::remove(const char *name) {
    return ::unlinkat(directory_, name, 0) == 0 ? 0 : errno;
}
int PosixFiles::free_bytes(uint64_t &bytes) const {
    struct statvfs status {};
    if (::fstatvfs(directory_, &status) != 0) {
        return errno;
    }
    const uint64_t unit = status.f_frsize;
    bytes = unit != 0 && status.f_bavail > UINT64_MAX / unit
                ? UINT64_MAX
                : static_cast<uint64_t>(status.f_bavail) * unit;
    return 0;
}
} // namespace ayni::hosts::native
