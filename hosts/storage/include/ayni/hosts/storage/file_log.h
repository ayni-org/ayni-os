// The shared Host persistent log (C-HOST-009, C-HOST-010, ADR-0016): the
// batches the kernel appended, in order, in one file of a state directory.
// The Host frames batches and uses their fixed identity for retry/trim;
// domain records remain opaque:
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
// New frames append at the end and each is synced before the next starts;
// uncertain bytes may be rewritten identically before confirmation. A power
// cut during a new append can tear its last frame. Opening checks every
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

#ifndef AYNI_HOSTS_STORAGE_FILE_LOG_H
#define AYNI_HOSTS_STORAGE_FILE_LOG_H

#include <array>
#include <ayni/hosts/storage/files.h>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ayni::hosts::storage {

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
        uint64_t cursor = 0;
    };

    // Files outlives the log and all blocking calls, including close/destruction.
    explicit FileLog(Files &files) : files_(files) {}
    ~FileLog() { close(); }
    FileLog(const FileLog &) = delete;
    FileLog &operator=(const FileLog &) = delete;

    // Opens directory/file_name, creating it when missing, locks it against
    // every other open log and checks every frame. Existing bytes are
    // re-confirmed before the first mutation, without writing during reads.
    // Only while closed. file_name must be flat and cannot be snapshot_0/snapshot_1
    // or use the value- prefix reserved for outer values/an adapter-reserved name; reserved names
    // are refused without data mutation.
    Opened open(const char *directory, const char *file_name);
    void close();

    // Framed bytes reserved by an accepted append, including its headers/trailer.
    static uint64_t append_bytes(uint32_t payload_bytes);

    // OK once the frame is synced to the device; FAILED when nothing of it
    // can ever be read, which includes every append to a damaged log;
    // UNKNOWN when the sync failed; a retry rescans and rewrites the entire
    // uncertain frame before proving it durable. size > 0.
    Result append(uint64_t commit_seq, const uint8_t *data, uint32_t size);

    // The whole stored batches whose commit_seq is at least from_commit_seq,
    // in log order, as many as fit; value is the bytes written. A read stops
    // before a frame it cannot return. An oversized first frame reports its
    // required bytes with READ_BUFFER_TOO_SMALL; corruption fails the read.
    Result read_prefix(uint64_t from_commit_seq, uint8_t *buffer, uint32_t size);
    // cursor is an opaque physical position, not a commit sequence. A trim
    // invalidates it; the caller restarts from 0 afterwards.
    Result read_log(uint64_t cursor, uint8_t *buffer, uint32_t size);
    Result fence(uint32_t writer_epoch);
    Result write_snapshot(uint32_t slot, uint32_t writer_epoch, uint64_t offset,
                          const uint8_t *data, uint32_t size);
    Result read_snapshot(uint32_t slot, uint64_t offset, uint8_t *buffer, uint32_t size);
    Result trim_prefix(uint32_t writer_epoch, uint64_t commit_seq, uint64_t transition_id);
    // I/O thread only. False when usable space cannot be established.
    bool free_bytes(uint64_t &available) const;
    uint32_t writer_epoch() const { return writer_epoch_; }
    void observe_writer(uint32_t epoch) { writer_epoch_ = epoch; }

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
    // Read-only schema refusal under the directory lock, before creating any new data file.
    Opened check_snapshot_schemas() const;
    Opened check_file_header(bool &initialized);
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
    bool same_payload(const Frame &frame, const uint8_t *data, uint32_t size, int &error) const;
    bool rescan_after_unknown();
    int read_up_to(int handle, uint64_t at, uint8_t *out, std::size_t size,
                   std::size_t &count) const;
    int read_exact(int handle, uint64_t at, uint8_t *out, std::size_t size) const;
    Result finish_trim();
    const char *snapshot_name(uint32_t slot) const;

    // One acquired namespace. The log token is absent only during open probes.
    struct OpenFiles {
        int log = Files::invalid_handle;
    };
    std::optional<OpenFiles> opened_; // Empty exactly when this engine owns no namespace.
    bool is_open() const { return opened_ && opened_->log != Files::invalid_handle; }
    int log_handle() const { return opened_ ? opened_->log : Files::invalid_handle; }
    Files &files_; // Exclusive namespace adapter; no OS/SDK representation lives here.
    std::array<char, 64> file_name_{};
    uint32_t writer_epoch_ = 1;
    uint64_t file_size_ = 0;   // at open, for the scan
    uint64_t end_ = 0;         // past the last whole frame; where the next frame goes
    bool damaged_ = false;     // the frame at end_ is damaged and a later frame follows
    bool tail_dirty_ = false;  // bytes past end_ that the next append truncates
    bool sync_failed_ = false; // failed sync, or an inherited prefix awaiting confirmation
    uint64_t uncertain_start_ = UINT64_MAX;
    // Every frame before read_from_ has a commit_seq of at most read_max_seq_,
    // so a read from a larger commit_seq starts there instead of at the top.
    uint64_t read_from_ = 0;
    uint64_t read_max_seq_ = 0;
};

} // namespace ayni::hosts::storage

#endif // AYNI_HOSTS_STORAGE_FILE_LOG_H
