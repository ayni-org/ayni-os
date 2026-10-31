// Native owns POSIX resources and the existing fault seam; the shared engine
// owns framing, scan/retry, snapshots, fencing and rotation. Member order keeps
// Files alive through FileLog's final close, including failed open cleanup.
#ifndef AYNI_HOSTS_NATIVE_FILE_LOG_H
#define AYNI_HOSTS_NATIVE_FILE_LOG_H
#include "posix_files.h"
#include <ayni/hosts/storage/file_log.h>
#include <ayni/hosts/storage/work_item.h>
namespace ayni::hosts::native {
class FileLog {
  public:
    using OpenStatus = storage::FileLog::OpenStatus;
    using Opened = storage::FileLog::Opened;
    using Result = storage::FileLog::Result;
    using Io = PosixFiles::Io;
    FileLog() : log_(files_) {}
    explicit FileLog(const Io &io) : files_(io), log_(files_) {}
    FileLog(const FileLog &) = delete;
    FileLog &operator=(const FileLog &) = delete;
    Opened open(const char *directory, const char *name) { return log_.open(directory, name); }
    void close() { log_.close(); }
    Result append(uint64_t sequence, const uint8_t *data, uint32_t size) {
        return log_.append(sequence, data, size);
    }
    Result read_prefix(uint64_t sequence, uint8_t *data, uint32_t size) {
        return log_.read_prefix(sequence, data, size);
    }
    Result read_log(uint64_t cursor, uint8_t *data, uint32_t size) {
        return log_.read_log(cursor, data, size);
    }
    Result fence(uint32_t epoch) { return log_.fence(epoch); }
    Result write_snapshot(uint32_t slot, uint32_t epoch, uint64_t offset, const uint8_t *data,
                          uint32_t size) {
        return log_.write_snapshot(slot, epoch, offset, data, size);
    }
    Result read_snapshot(uint32_t slot, uint64_t offset, uint8_t *data, uint32_t size) {
        return log_.read_snapshot(slot, offset, data, size);
    }
    Result trim_prefix(uint32_t epoch, uint64_t sequence, uint64_t transition) {
        return log_.trim_prefix(epoch, sequence, transition);
    }
    bool free_bytes(uint64_t &bytes) const { return log_.free_bytes(bytes); }
    // Preserve the composition owner while executing the same request mechanism as ESP.
    storage::WorkResult serve(const storage::WorkItem &request) {
        return storage::serve_work(log_, files_, request);
    }
    uint32_t writer_epoch() const { return log_.writer_epoch(); }
    void observe_writer(uint32_t epoch) { log_.observe_writer(epoch); }

  private:
    PosixFiles files_;     // Sole Native descriptors, exclusion and syscall owner.
    storage::FileLog log_; // Borrows files_; closes before its adapter is destroyed.
};
} // namespace ayni::hosts::native
#endif
