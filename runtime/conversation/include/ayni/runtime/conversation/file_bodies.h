// Portable body store (ADR-0096) on the Host's outer value files. A body is one or more value
// files named by an identity derived from its scope and digest, so an identical write after a
// restart finds the same files. Writes, loads and removals go through the bounded Host queue:
// write answers NOT_READY until every file is durable, read answers NOT_READY until loaded, and
// ready() turns nonzero once nothing is outstanding. Durable cached bytes can be evicted without
// deleting their files. Any thread may call the descriptor. Only the thread that called configure,
// the Host's owner thread, submits file requests: work recorded on another thread, or refused by a
// full queue, waits for the owner's next call, ready() included. Completions run on the storage
// worker. The object lives through Host join once any request was accepted.
#ifndef AYNI_RUNTIME_CONVERSATION_FILE_BODIES_H
#define AYNI_RUNTIME_CONVERSATION_FILE_BODIES_H
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/conversation/body_collection.h>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace ayni::runtime {
class FileBodies {
  public:
    // Value files use owner identities from base upward, one per chunk, so base must leave
    // room above it and must not collide with another owner of the same file port.
    static constexpr uint64_t default_base = UINT64_C(0x4159424f00000000);
    FileBodies() = default;
    FileBodies(const FileBodies &) = delete;
    FileBodies &operator=(const FileBodies &) = delete;
    // On the owner thread, before any other call.
    ayni_status_t configure(hosts::storage::FileValuePort files, uint64_t base = default_base);
    const ayni_body_store_v1 *store() const { return &store_; }
    BodyCollection collection() { return {store(), this, collect_port, ready}; }
    // Owner-thread before use: zero keeps the existing unlimited cache. A positive budget
    // bounds cached payload bytes, including active loads/writes; metadata is separate.
    // Oversized bodies return CAPACITY_EXCEEDED; active borrows are never evicted.
    ayni_status_t set_cache_limit(size_t bytes);
    // Any thread. Evicts only durable, unborrowed cached copies, preserving disk files and
    // descriptor identity. BUSY means active entries prevent reaching the requested target.
    ayni_status_t trim_cache(size_t target_bytes = 0);
    size_t cached_bytes() const;
    // Nonzero when no body is waiting for its files; the shape of a HistoryPort ready callback.
    static uint32_t ready(void *context, uint64_t task);
    // Releases every body of scope written or loaded by this process that keep does not name.
    void collect(uint64_t scope, const std::vector<ayni_body_ref_v1> &keep);

  private:
    enum class State { writing, loading, durable, failed };
    struct Entry {
        uint64_t scope = 0;
        uint64_t digest = 0;
        std::string bytes;
        State state = State::writing;
        ayni_status_t failure = AYNI_OK; // The first error, in failed.
        uint32_t submitted = 0;          // Chunks accepted by the queue, in order.
        uint32_t outstanding = 0;        // Accepted file requests not yet completed.
        bool released = false;           // Erase once outstanding reaches zero.
    };
    struct Pending {
        FileBodies *self = nullptr;
        uint64_t id = 0;
        uint32_t chunk = 0;
        uint32_t expected = 0; // Read: exact chunk bytes.
        bool removal = false;  // Removals never touch a cached entry.
        hosts::storage::FileValueRequest request;
    };
    static ayni_status_t write(void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *);
    static ayni_status_t read(void *, const ayni_body_ref_v1 *, uint8_t *, uint32_t);
    static void release(void *, const ayni_body_ref_v1 *);
    static void collect_port(void *context, uint64_t scope,
                             const std::vector<ayni_body_ref_v1> &keep) {
        static_cast<FileBodies *>(context)->collect(scope, keep);
    }
    size_t cached_bytes_locked() const;
    ayni_status_t trim_locked(size_t target);
    ayni_status_t reserve_cache(size_t bytes);
    static void complete(void *, const hosts::storage::FileValueResult &) noexcept;
    static uint64_t identity(uint64_t scope, uint64_t digest);
    static uint32_t chunks(uint32_t size);
    // Under the lock: marks a body as waiting for its files.
    void wait(uint64_t id);
    bool removing(uint64_t id) const;
    Pending *prepare(hosts::storage::FileValueRequest request, uint64_t id, uint32_t chunk,
                     uint32_t expected);
    // On the owner thread, without the lock: submits removals first, then waiting chunks.
    void flush();
    void withdraw(const std::vector<Pending *> &requests, std::size_t from);
    hosts::storage::FileValuePort files_{};
    uint64_t base_ = default_base;
    size_t cache_limit_ = 0; // Zero is unlimited; never includes externally owned file buffers.
    std::thread::id owner_;
    mutable std::mutex mutex_;
    std::map<uint64_t, Entry> entries_;
    std::vector<uint64_t> waiting_;                      // Entries with chunks to submit.
    std::vector<hosts::storage::FileValueKey> removals_; // Files to remove, not yet accepted.
    std::list<Pending> pending_;                         // Stable completion contexts.
    uint32_t outstanding_ = 0;
    ayni_body_store_v1 store_{sizeof(ayni_body_store_v1), 0, this, write, read, release};
};
} // namespace ayni::runtime
#endif
