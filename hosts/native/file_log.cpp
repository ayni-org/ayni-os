#include "file_log.h"

#include <ayni/ayni.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ayni::hosts::native {
namespace {

constexpr std::array<uint8_t, 4> file_magic{{'A', 'Y', 'N', 'L'}};
constexpr uint32_t file_format_version = 1;
constexpr std::size_t file_header_size = 8;
constexpr std::array<uint8_t, 4> frame_magic{{'A', 'Y', 'N', 'F'}};
constexpr std::size_t frame_header_size = 20;
constexpr std::size_t frame_header_crc_at = 16;
constexpr std::size_t frame_trailer_size = 4;
// Payloads are checked through a buffer of this size on the stack.
constexpr std::size_t chunk_size = 4096;

constexpr std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t index = 0; index < table.size(); ++index) {
        uint32_t crc = index;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? 0xEDB88320U ^ (crc >> 1U) : crc >> 1U;
        }
        table[index] = crc;
    }
    return table;
}

constexpr std::array<uint32_t, 256> crc_table = make_crc_table();

// CRC-32/ISO-HDLC; passing a previous result continues it over more bytes.
uint32_t crc32(const uint8_t *data, std::size_t size, uint32_t previous = 0) {
    uint32_t crc = ~previous;
    for (std::size_t index = 0; index < size; ++index) {
        crc = crc_table[(crc ^ data[index]) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

void store_le32(uint8_t *out, uint32_t value) {
    for (std::size_t index = 0; index < 4U; ++index) {
        out[index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

void store_le64(uint8_t *out, uint64_t value) {
    for (std::size_t index = 0; index < 8U; ++index) {
        out[index] = static_cast<uint8_t>(value >> (8U * index));
    }
}

uint32_t load_le32(const uint8_t *in) {
    uint32_t value = 0;
    for (std::size_t index = 0; index < 4U; ++index) {
        value |= static_cast<uint32_t>(in[index]) << (8U * index);
    }
    return value;
}

uint64_t load_le64(const uint8_t *in) {
    uint64_t value = 0;
    for (std::size_t index = 0; index < 8U; ++index) {
        value |= static_cast<uint64_t>(in[index]) << (8U * index);
    }
    return value;
}

std::array<uint8_t, file_header_size> file_header() {
    std::array<uint8_t, file_header_size> header{};
    std::memcpy(header.data(), file_magic.data(), file_magic.size());
    store_le32(header.data() + file_magic.size(), file_format_version);
    return header;
}

// The system calls below return 0 or the errno of the call that failed.

int read_exact(int fd, uint64_t at, uint8_t *out, std::size_t size) {
    while (size > 0U) {
        const ssize_t got = ::pread(fd, out, size, static_cast<off_t>(at));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return errno;
        }
        if (got == 0) {
            return EIO; // the file ends before the bytes it was checked to hold
        }
        const auto count = static_cast<std::size_t>(got);
        out += count;
        at += count;
        size -= count;
    }
    return 0;
}

int write_all(int fd, uint64_t at, const uint8_t *data, std::size_t size) {
    while (size > 0U) {
        const ssize_t put = ::pwrite(fd, data, size, static_cast<off_t>(at));
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
        at += count;
        size -= count;
    }
    return 0;
}

// The file's data, and the size that reaches it, on the device (C-HOST-010).
// On macOS fsync stops at the drive's cache and F_FULLFSYNC does not; that
// branch is not yet built or tested on macOS.
int sync_file(int fd) {
#if defined(__APPLE__)
    return ::fcntl(fd, F_FULLFSYNC) == 0 ? 0 : errno;
#else
    while (::fdatasync(fd) != 0) {
        if (errno != EINTR) {
            return errno;
        }
    }
    return 0;
#endif
}

} // namespace

FileLog::Opened FileLog::open(const char *directory, const char *file_name) {
    const int directory_fd = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory_fd < 0) {
        return {OpenStatus::file_error, errno};
    }
    fd_ = ::openat(directory_fd, file_name, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    Opened opened =
        fd_ < 0 ? Opened{OpenStatus::file_error, errno} : check_file_header(directory_fd);
    if (opened.status == OpenStatus::ok) {
        opened = scan();
    }
    ::close(directory_fd);
    if (opened.status != OpenStatus::ok) {
        close();
    }
    return opened;
}

void FileLog::close() {
    if (fd_ >= 0) {
        ::close(fd_); // also releases the lock
    }
    fd_ = -1;
    file_size_ = 0;
    end_ = 0;
    damaged_ = false;
    tail_dirty_ = false;
    sync_failed_ = false;
    read_from_ = 0;
    read_max_seq_ = 0;
}

FileLog::Opened FileLog::check_file_header(int directory_fd) {
    // One writer per file, or two Hosts would interleave their appends.
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        const int error = errno;
        return {error == EWOULDBLOCK ? OpenStatus::in_use : OpenStatus::file_error, error};
    }
    struct stat file_status {};
    if (::fstat(fd_, &file_status) != 0) {
        return {OpenStatus::file_error, errno};
    }
    file_size_ = static_cast<uint64_t>(file_status.st_size);

    const std::array<uint8_t, file_header_size> expected = file_header();
    std::array<uint8_t, file_header_size> found{};
    const std::size_t present =
        static_cast<std::size_t>(std::min<uint64_t>(file_size_, found.size()));
    int error = read_exact(fd_, 0, found.data(), present);
    if (error != 0) {
        return {OpenStatus::file_error, error};
    }
    if (std::memcmp(found.data(), expected.data(), present) != 0) {
        return {OpenStatus::not_a_log, 0};
    }
    if (present == expected.size()) {
        return {OpenStatus::ok, 0};
    }
    // A new file, or one whose creation a power cut tore: nothing is appended
    // before the header is synced, so it holds no batch.
    error = write_all(fd_, 0, expected.data(), expected.size());
    if (error == 0) {
        error = sync_file(fd_);
    }
    if (error == 0 && ::fsync(directory_fd) != 0) { // the new directory entry
        error = errno;
    }
    if (error != 0) {
        return {OpenStatus::file_error, error};
    }
    file_size_ = expected.size();
    return {OpenStatus::ok, 0};
}

FileLog::Opened FileLog::scan() {
    uint64_t at = file_header_size;
    while (at < file_size_) {
        Frame frame{};
        int error = 0;
        const FrameCheck check = check_frame(at, frame, error);
        if (check == FrameCheck::whole) {
            at = frame.end;
            continue;
        }
        if (check == FrameCheck::bad_header) {
            bool later = false;
            error = find_later_header(at, later);
            damaged_ = later;
        } else if (check == FrameCheck::bad_payload) {
            // The header is sound, so the frame's extent is known: anything
            // after it was appended once it was complete.
            damaged_ = frame.end < file_size_;
        }
        if (error != 0) {
            return {OpenStatus::file_error, error};
        }
        break;
    }
    end_ = at;
    tail_dirty_ = at < file_size_;
    read_from_ = file_header_size;
    read_max_seq_ = 0;
    return {OpenStatus::ok, 0};
}

FileLog::FrameCheck FileLog::check_frame(uint64_t at, Frame &frame, int &error) const {
    if (file_size_ - at < frame_header_size) {
        return FrameCheck::past_end;
    }
    if (!read_header(at, frame, error)) {
        return error != 0 ? FrameCheck::io_error : FrameCheck::bad_header;
    }
    if (frame.end > file_size_) {
        return FrameCheck::past_end;
    }
    if (!check_payload(frame, nullptr, error)) {
        return error != 0 ? FrameCheck::io_error : FrameCheck::bad_payload;
    }
    return FrameCheck::whole;
}

// Each later offset that starts with the frame magic is a candidate. A
// payload that happens to hold a sound frame header can make a torn tail
// look damaged; that restricts the kernel instead of losing a batch.
int FileLog::find_later_header(uint64_t at, bool &found) const {
    found = false;
    std::array<uint8_t, chunk_size> chunk{};
    uint64_t from = at + 1U;
    while (file_size_ - from >= frame_header_size) {
        const auto size =
            static_cast<std::size_t>(std::min<uint64_t>(chunk.size(), file_size_ - from));
        int error = read_exact(fd_, from, chunk.data(), size);
        if (error != 0) {
            return error;
        }
        for (std::size_t index = 0; index + frame_magic.size() <= size; ++index) {
            if (std::memcmp(chunk.data() + index, frame_magic.data(), frame_magic.size()) != 0) {
                continue;
            }
            if (file_size_ - (from + index) < frame_header_size) {
                return 0; // no later offset has room for a header either
            }
            Frame frame{};
            if (read_header(from + index, frame, error)) {
                found = true;
                return 0;
            }
            if (error != 0) {
                return error;
            }
        }
        // A magic that straddles the end of this chunk starts the next one.
        from += size - (frame_magic.size() - 1U);
    }
    return 0;
}

bool FileLog::read_header(uint64_t at, Frame &frame, int &error) const {
    std::array<uint8_t, frame_header_size> header{};
    error = read_exact(fd_, at, header.data(), header.size());
    if (error != 0 || std::memcmp(header.data(), frame_magic.data(), frame_magic.size()) != 0 ||
        load_le32(header.data() + frame_header_crc_at) !=
            crc32(header.data(), frame_header_crc_at)) {
        return false;
    }
    frame.payload_size = load_le32(header.data() + 4);
    frame.commit_seq = load_le64(header.data() + 8);
    frame.payload_at = at + frame_header_size;
    frame.end = frame.payload_at + frame.payload_size + frame_trailer_size;
    return true;
}

// Into out when it is not null, else through a chunk on the stack.
bool FileLog::check_payload(const Frame &frame, uint8_t *out, int &error) const {
    std::array<uint8_t, chunk_size> chunk{};
    uint32_t crc = 0;
    for (uint32_t done = 0; done < frame.payload_size;) {
        uint8_t *into = out != nullptr ? out : chunk.data();
        const uint32_t size = out != nullptr
                                  ? frame.payload_size
                                  : std::min<uint32_t>(chunk_size, frame.payload_size - done);
        error = read_exact(fd_, frame.payload_at + done, into, size);
        if (error != 0) {
            return false;
        }
        crc = crc32(into, size, crc);
        done += size;
    }
    std::array<uint8_t, frame_trailer_size> trailer{};
    error = read_exact(fd_, frame.end - frame_trailer_size, trailer.data(), trailer.size());
    return error == 0 && load_le32(trailer.data()) == crc;
}

FileLog::Result FileLog::append(uint64_t commit_seq, const uint8_t *data, uint32_t size) {
    if (damaged_ || sync_failed_ || (tail_dirty_ && !truncate_tail())) {
        return {AYNI_WRITE_FAILED, 0};
    }
    std::array<uint8_t, frame_header_size> header{};
    std::memcpy(header.data(), frame_magic.data(), frame_magic.size());
    store_le32(header.data() + 4, size);
    store_le64(header.data() + 8, commit_seq);
    store_le32(header.data() + frame_header_crc_at, crc32(header.data(), frame_header_crc_at));
    std::array<uint8_t, frame_trailer_size> trailer{};
    store_le32(trailer.data(), crc32(data, size));

    const uint64_t payload_at = end_ + frame_header_size;
    if (write_all(fd_, end_, header.data(), header.size()) != 0 ||
        write_all(fd_, payload_at, data, size) != 0 ||
        write_all(fd_, payload_at + size, trailer.data(), trailer.size()) != 0) {
        // What reached the file is an incomplete frame past end_: never read,
        // and truncated before the next append.
        tail_dirty_ = true;
        return {AYNI_WRITE_FAILED, 0};
    }
    if (!sync()) {
        // The frame may be on the device or not. Reads keep ending at end_;
        // reopening the log finds out.
        tail_dirty_ = true;
        return {AYNI_WRITE_UNKNOWN, 0};
    }
    end_ = payload_at + size + frame_trailer_size;
    return {AYNI_WRITE_OK, 0};
}

FileLog::Result FileLog::read_prefix(uint64_t from_commit_seq, uint8_t *buffer, uint32_t size) {
    uint64_t at = file_header_size;
    uint64_t max_seq = 0;
    if (from_commit_seq > read_max_seq_) {
        at = read_from_;
        max_seq = read_max_seq_;
    }
    uint32_t written = 0;
    bool stopped = false;
    while (at < end_) {
        Frame frame{};
        int error = 0;
        // Frames before end_ were whole when opened or written; checking
        // them again catches the medium changing since.
        if (!read_header(at, frame, error) || frame.end > end_) {
            stopped = true;
            break;
        }
        if (frame.commit_seq >= from_commit_seq) {
            if (frame.payload_size > size - written ||
                !check_payload(frame, buffer + written, error)) {
                stopped = true;
                break;
            }
            written += frame.payload_size;
        }
        max_seq = std::max(max_seq, frame.commit_seq);
        at = frame.end;
    }
    read_from_ = at;
    read_max_seq_ = max_seq;
    // In a damaged log, end_ is where the damaged frame starts.
    if ((stopped || damaged_) && written == 0U) {
        return {AYNI_WRITE_FAILED, 0};
    }
    return {AYNI_WRITE_OK, written};
}

bool FileLog::truncate_tail() {
    if (::ftruncate(fd_, static_cast<off_t>(end_)) != 0 || !sync()) {
        return false;
    }
    tail_dirty_ = false;
    return true;
}

bool FileLog::sync() {
    if (sync_file(fd_) != 0) {
        // After a failed sync the kernel may have dropped the dirty pages, so
        // a later sync proves nothing about them: no more appends.
        sync_failed_ = true;
        return false;
    }
    return true;
}

} // namespace ayni::hosts::native
