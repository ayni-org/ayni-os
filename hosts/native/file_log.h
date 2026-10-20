// The Native Host's persistent log (C-HOST-009, C-HOST-010, ADR-0016): the
// batches the kernel appended, in order, in one file of a state directory.
// The Host frames each batch itself and never interprets the batch bytes:
//
//   file header  "AYNL", format version (uint32, 1)
//   frame        "AYNF", payload length (uint32), commit_seq (uint64),
//                CRC-32 of these 16 bytes (uint32),
//                payload (the encoded batch), CRC-32 of the payload (uint32)
//
// Integers are little-endian; CRC-32 is CRC-32/ISO-HDLC, as in
// contracts/storage/records.md. The header CRC makes the length and
// commit_seq trustworthy; the payload CRC, written after the data, is what
// proves a frame complete, which a length prefix alone cannot (ayni.h).
//
// Appends only ever write at the end and each one is synced before the next
// starts, so a power cut can tear only the last frame. Opening checks every
// frame. A frame that is not whole is that torn tail when nothing was
// written after it: the file ends inside it or right at its end, or, when
// its own header does not check out, no header that does follows it. Reads
// end before it and the next append truncates it. Any other frame that is
// not whole was damaged after it was synced: a read that reaches it fails
// and appends fail, so the kernel becomes RESTRICTED rather than lose the
// batches behind it (C-STORE-009). A last frame damaged on the medium cannot
// be told from a torn one and is dropped.
//
// Not thread-safe. open() runs on the caller's thread; afterwards the Host's
// I/O thread alone calls it until the Host joins that thread and closes the
// log. Every call may block on the file.

#ifndef AYNI_HOSTS_NATIVE_FILE_LOG_H
#define AYNI_HOSTS_NATIVE_FILE_LOG_H

#include <cstddef>
#include <cstdint>

namespace ayni::hosts::native {

class FileLog {
  public:
    enum class OpenStatus {
        ok,
        in_use,     // another open log holds the file's lock
        not_a_log,  // the file does not start with this format's header
        file_error, // a file call failed; error is its errno
    };

    struct Opened {
        OpenStatus status;
        int error;
    };

    // A completion's outcome and value (ayni.h, AYNI_WRITE_*).
    struct Result {
        uint32_t outcome;
        uint64_t value;
    };

    FileLog() = default;
    ~FileLog() { close(); }
    FileLog(const FileLog &) = delete;
    FileLog &operator=(const FileLog &) = delete;

    // Opens directory/file_name, creating it when missing, locks it against
    // every other open log and checks every frame. Only while closed.
    Opened open(const char *directory, const char *file_name);
    void close();

    // OK once the frame is synced to the device; FAILED when nothing of it
    // can ever be read, which includes every append to a damaged log;
    // UNKNOWN when the sync failed, after which every append fails until the
    // log is opened again. size > 0.
    Result append(uint64_t commit_seq, const uint8_t *data, uint32_t size);

    // The whole stored batches whose commit_seq is at least from_commit_seq,
    // in log order, as many as fit; value is the bytes written. A read stops
    // before a frame it cannot return, and fails if that frame is the first.
    Result read_prefix(uint64_t from_commit_seq, uint8_t *buffer, uint32_t size);

  private:
    struct Frame {
        uint64_t commit_seq;
        uint32_t payload_size;
        uint64_t payload_at;
        uint64_t end; // offset past the frame
    };

    enum class FrameCheck { whole, past_end, bad_header, bad_payload, io_error };

    // The functions below that take int &error, or return an int, report
    // the errno of a failed file call there, and 0 otherwise.
    Opened check_file_header(int directory_fd);
    Opened scan();
    FrameCheck check_frame(uint64_t at, Frame &frame, int &error) const;
    // Whether a frame header that checks out starts anywhere after at: the
    // trace of an append made after the frame at at was complete.
    int find_later_header(uint64_t at, bool &found) const;
    // False for a header that does not check out, or on a file error.
    bool read_header(uint64_t at, Frame &frame, int &error) const;
    // Reads the payload and checks it against the frame's trailing CRC.
    bool check_payload(const Frame &frame, uint8_t *out, int &error) const;
    bool truncate_tail();
    bool sync();

    int fd_ = -1;
    uint64_t file_size_ = 0;  // at open, for the scan
    uint64_t end_ = 0;        // past the last whole frame; where the next frame goes
    bool damaged_ = false;    // the frame at end_ is damaged and a later frame follows
    bool tail_dirty_ = false; // bytes past end_ that the next append truncates
    bool sync_failed_ = false;
    // Every frame before read_from_ has a commit_seq of at most read_max_seq_,
    // so a read from a larger commit_seq starts there instead of at the top.
    uint64_t read_from_ = 0;
    uint64_t read_max_seq_ = 0;
};

} // namespace ayni::hosts::native

#endif // AYNI_HOSTS_NATIVE_FILE_LOG_H
