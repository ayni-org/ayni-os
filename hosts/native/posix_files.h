// Native file-system implementation. Only this adapter owns POSIX descriptors,
// directory/file locks and syscall retry details; the shared log owns framing.
#ifndef AYNI_HOSTS_NATIVE_POSIX_FILES_H
#define AYNI_HOSTS_NATIVE_POSIX_FILES_H
#include <ayni/hosts/storage/files.h>

namespace ayni::hosts::native {
class PosixFiles final : public storage::Files {
  public:
    // Existing Native deterministic write/sync fault seams; Core and ESP do not depend on them.
    struct Io {
        void *context = nullptr;
        int (*write)(void *, int, uint64_t, const uint8_t *, std::size_t) = nullptr;
        int (*sync_file)(void *, int) = nullptr;
        int (*sync_directory)(void *, int) = nullptr;
    };
    PosixFiles() = default;
    explicit PosixFiles(const Io &io) : io_(io) {}
    ~PosixFiles() override { end(); }
    PosixFiles(const PosixFiles &) = delete;
    PosixFiles &operator=(const PosixFiles &) = delete;
    int begin(const char *) override;
    void end() override;
    int open(const char *, Mode, int &) override;
    void close(int) override;
    int lock(int) override;
    int read(int, uint64_t, uint8_t *, std::size_t, std::size_t &) const override;
    int write(int, uint64_t, const uint8_t *, std::size_t) override;
    int size(int, uint64_t &) const override;
    int truncate(int, uint64_t) override;
    int sync_file(int) override;
    int sync_namespace() override;
    int rename(const char *, const char *) override;
    int remove(const char *) override;
    int free_bytes(uint64_t &) const override;

  private:
    Io io_{}; // Immutable borrowed fault callbacks; absent uses real syscalls.
    int directory_ = invalid_handle; // POSIX namespace fd, absent before successful begin.
    int lock_ = invalid_handle;      // Exclusive directory lock fd; released by end.
};
} // namespace ayni::hosts::native
#endif
