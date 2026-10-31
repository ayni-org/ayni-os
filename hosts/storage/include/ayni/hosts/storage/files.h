// File-system boundary for the shared Host log. It owns no Core state or threads.
#ifndef AYNI_HOSTS_STORAGE_FILES_H
#define AYNI_HOSTS_STORAGE_FILES_H
#include <cstddef>
#include <cstdint>

namespace ayni::hosts::storage {
// A blocking file namespace, exclusively owned between begin/end. open runs on
// the configuring thread; afterwards one storage worker uses it until joined.
// Calls never reenter the engine. Handles are adapter tokens, never Core/ABI values. Operations
// return 0 or an errno-compatible cause; ENOENT and EWOULDBLOCK have their usual classifications.
class Files {
  public:
    static constexpr int invalid_handle = -1;
    enum class Mode { read_existing, read_existing_no_follow, read_write_create, replace };
    virtual ~Files() = default;
    // Opens and excludes this namespace from other writers; partial failure cleans up.
    virtual int begin(const char *name) = 0;
    // Closes namespace resources after every file handle is closed; idempotent.
    virtual void end() = 0;
    // Names are flat relative names. Adapter-reserved names are refused by all file
    // mutations/opens (e.g. a namespace lock file). Success publishes one live handle only.
    virtual int open(const char *name, Mode mode, int &handle) = 0;
    virtual void close(int handle) = 0;
    // Excludes legacy writers that lock individual files. A namespace-exclusive
    // adapter without other file users already satisfies this operation.
    virtual int lock(int handle) = 0;
    // Successful reads may stop at EOF; error leaves count zero. No borrowed bytes survive return.
    virtual int read(int handle, uint64_t offset, uint8_t *data, std::size_t bytes,
                     std::size_t &count) const = 0;
    // Complete positional write, or a cause that may have left uncertain bytes.
    virtual int write(int handle, uint64_t offset, const uint8_t *data, std::size_t bytes) = 0;
    virtual int size(int handle, uint64_t &bytes) const = 0;
    virtual int truncate(int handle, uint64_t bytes) = 0;
    // Successful sync confirms file bytes and length to the backing medium.
    virtual int sync_file(int handle) = 0;
    // Confirms namespace changes. An adapter may satisfy this through already
    // synchronous durable namespace operations; it must document that guarantee.
    virtual int sync_namespace() = 0;
    // Atomic name replacement within this namespace; durability is confirmed separately.
    // Success keeps source tokens associated with the new name for subsequent write/sync.
    // Tokens for the replaced destination may retain the old, now unlinked file.
    virtual int rename(const char *from, const char *to) = 0;
    virtual int remove(const char *name) = 0;
    // Actual currently usable bytes, before the asynchronous Host's pending reservations.
    virtual int free_bytes(uint64_t &bytes) const = 0;
};
} // namespace ayni::hosts::storage
#endif
