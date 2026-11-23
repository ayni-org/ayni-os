// Bounded, immutable bodies over asynchronous Host value files (ADR-0104).
// Scope references are independent owners; a durable manifest names every owner of
// one shared payload. Only the configure thread submits I/O. Worker completions
// borrow stable buffers, and this object lives through Host close/join.
#ifndef AYNI_RUNTIME_CONVERSATION_FILE_BODIES_H
#define AYNI_RUNTIME_CONVERSATION_FILE_BODIES_H
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/conversation/body_collection.h>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
namespace ayni::runtime {
class FileBodies {
  public:
    static constexpr uint64_t default_base = UINT64_C(0x4159424f00000000);
    FileBodies() = default;
    FileBodies(const FileBodies &) = delete;
    FileBodies &operator=(const FileBodies &) = delete;
    ayni_status_t configure(hosts::storage::FileValuePort files, size_t cache_bytes,
                            uint64_t base = default_base);
    const ayni_body_store_v1 *store() const { return &store_; }
    void set_shared_call_results() { store_.reserved |= AYNI_BODY_STORE_SHARED_RESULTS; }
    BodyCollection collection() {
        return {store(), this, collect_port, ready, status, collected, progress};
    }
    // Every configured store has a positive payload budget; zero is rejected.
    // Accepted, unsaved bytes and worker borrows cannot be reclaimed.
    ayni_status_t set_cache_limit(size_t bytes);
    ayni_status_t trim_cache(size_t target_bytes = 0);
    size_t cached_bytes() const;
    // Progress/drain only; it never hides errors returned by read/stage/confirm.
    static uint32_t ready(void *context, uint64_t task);
    // Owner progress only; the return value permits the existing strategy callback shape.
    static uint32_t progress(void *context, uint64_t task);
    // Owner-thread scope query: BUSY until its releases are durable; sticky failures propagate.
    static ayni_status_t collected(void *context, uint64_t scope);
    // Pure queue query: no worker borrow and all work drained, or blocked by a sticky failure.
    // Unconfirmed chat tails need no persistence. Always check status for a successful close.
    bool idle() const;
    // Owner-thread pacing hint: defers purely background persistence and cleanup while resident
    // bytes stay at or below half of cache_limit_, leaving Host I/O uncontended for hot Core
    // commits. Foreground waiters (read/share/put/write returning NOT_READY) still advance.
    // Clearing the pause scans and submits deferred work immediately.
    void set_background_writes_paused(bool paused);
    static ayni_status_t status(void *context);
    void collect(uint64_t scope, const std::vector<ayni_body_ref_v1> &keep);

  private:
    static constexpr uint32_t chunk_bytes = 4096; // Background quantum, independent of AYFV max.
    static constexpr uint32_t owner_limit = 64;
    static constexpr uint32_t manifest_header = 32;
    static constexpr uint32_t owner_bytes = 16;
    static constexpr uint32_t manifest_bytes = manifest_header + owner_limit * owner_bytes;
    static constexpr uint32_t manifest_magic = 0x424e5941; // AYNB; ADR-0104 body format 1.
    static constexpr uint32_t manifest_format = 1;
    static constexpr size_t entry_limit = 4096; // Bounds ownership metadata independently of bytes.
    enum class State { checking, writing, loading, ready, durable, evicted, failed };
    struct Owner {
        uint64_t scope = 0;
        uint64_t id = 0;
    };
    struct Entry {
        struct Release {
            uint64_t id; // Removed scope alias; retained until its durable absence is proved.
            std::optional<uint64_t> revision; // None until the ownership manifest is loaded.
        };
        uint64_t digest = 0;
        uint32_t size = 0;
        std::string bytes; // One immutable resident payload, shared by all owners.
        State state = State::checking;
        ayni_status_t failure = AYNI_OK;
        std::vector<Owner> owners;       // Desired ownership; loaded before a durable confirmation.
        std::vector<uint64_t> temporary; // New scope owners not yet explicitly confirmed.
        std::vector<Owner> additions;    // Staged owners while the disk manifest is being read.
        std::vector<Release>
            releases;            // Bounded ownership-removal fences, including cold manifests.
        uint64_t generation = 0; // Last confirmed manifest CAS generation; zero if absent.
        uint32_t saved_manifest_bytes = 0; // Last observed durable image size, zero if absent.
        uint64_t revision = 0, saved_revision = 0; // Owner changes vs confirmed manifest.
        uint32_t chunk = 0;                        // Next payload chunk to load/write/remove.
        bool manifest_loaded = false;
        bool pending = false;           // Exactly one worker borrow per entry.
        bool persist_requested = false; // confirm schedules persistence; stage alone owns only RAM.
        bool load_payload = false; // Only read requests payload RAM; metadata operations do not.
        bool staged = false;       // Bytes were accepted locally, so reads need no disk load.
        bool deleting = false;     // All owners released; content then manifest are removed.
        bool awaited = false;      // Foreground caller saw NOT_READY; bypasses background pause.
    };
    enum class Operation { manifest_read, manifest_write, payload_read, payload_write, remove };
    struct Pending {
        FileBodies *self = nullptr;
        uint64_t id = 0;
        Operation operation = Operation::manifest_read;
        uint64_t revision = 0;
        std::string buffer; // Private immutable write image or exclusive read buffer.
        hosts::storage::FileValueRequest request;
    };
    static ayni_status_t stage(void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *);
    static ayni_status_t put(void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *);
    static ayni_status_t write(void *, uint64_t, ayni_bytes_v1, ayni_body_ref_v1 *);
    static ayni_status_t share(void *, uint64_t, const ayni_body_ref_v1 *, ayni_body_ref_v1 *);
    static ayni_status_t confirm(void *, const ayni_body_ref_v1 *);
    static ayni_status_t read(void *, const ayni_body_ref_v1 *, uint8_t *, uint32_t);
    static void release(void *, const ayni_body_ref_v1 *);
    static void release_temporary(void *, const ayni_body_ref_v1 *);
    static void complete(void *, const hosts::storage::FileValueResult &) noexcept;
    static void collect_port(void *context, uint64_t scope,
                             const std::vector<ayni_body_ref_v1> &keep) {
        static_cast<FileBodies *>(context)->collect(scope, keep);
    }
    static uint64_t identity(uint64_t scope, uint64_t digest);
    static uint32_t chunks(uint32_t size);
    static bool owns(const Entry &, uint64_t id);
    static bool valid_ref(const ayni_body_ref_v1 *);
    ayni_status_t add_owner(Entry &, uint64_t scope, uint64_t id);
    void drop_owner(Entry &, uint64_t id);
    bool apply_release(Entry &, Entry::Release &);
    static void retire_releases(Entry &);
    bool decode_manifest(Entry &, const std::string &);
    static std::string encode_manifest(const Entry &);
    bool conflicting(const ayni_body_ref_v1 &) const;
    Entry *find(const ayni_body_ref_v1 &);
    Entry *metadata(const ayni_body_ref_v1 &); // Caller rejected digest collisions under the lock.
    size_t cached_bytes_locked() const;
    ayni_status_t trim_locked(size_t target, bool completed);
    ayni_status_t reserve_cache(size_t bytes);
    // Owner thread: at most one chunk in flight across the store, leaving queue room for Core.
    void flush();
    bool prepare(uint64_t id, Entry &);
    hosts::storage::FileValuePort files_{};
    uint64_t base_ = default_base;
    size_t cache_limit_ =
        0; // Zero only before configure; every configured store has a finite budget.
    std::thread::id owner_;
    mutable std::mutex mutex_;
    std::map<uint64_t, Entry> entries_; // Content identity -> payload and durable owner manifest.
    std::set<uint64_t>
        expired_; // Temporary input IDs awaiting their original Call retirement release.
    std::list<Pending> pending_; // Stable callback buffers through completion.
    bool scan_needed_ = true;    // Locked. False only after a full scan found no eligible request.
    bool background_writes_paused_ = false; // Locked. Owner hint; awaited entries/pressure bypass.
    ayni_status_t failure_ = AYNI_OK;       // Sticky background persistence failure.
    ayni_body_store_v1 store_{
        sizeof(ayni_body_store_v1), 0, this, write, read, release, stage, share, confirm,
        release_temporary};
};
} // namespace ayni::runtime
#endif
