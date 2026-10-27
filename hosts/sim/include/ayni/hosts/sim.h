// Simulation Host for tests and the simulator (MA-003, MA-005): time moves
// only when the test advances it (FakeClock, C-TIME-001), wakes are counted
// instead of scheduling a thread, and misuse of the ingress lock is recorded
// so that tests can assert the kernel never calls the Host while holding it
// (C-HOST-020). The lock is a real mutex, so producer threads may be used.
//
// Storage is an in-memory log. Requests stay pending until the test calls
// complete_storage(), which reads or fills the kernel's buffer only then, as
// a Host I/O thread would (C-HOST-019). Faults are injected per request, and
// power_cut() ends the kernel's requests without a result, as power loss
// would, leaving each pending append lost, stored or torn.

#ifndef AYNI_HOSTS_SIM_H
#define AYNI_HOSTS_SIM_H

#include <ayni/ayni.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace ayni::hosts::sim {

// One batch as the Host frames it: commit_seq as the kernel named it at
// append time, and the encoded bytes. A torn batch holds only the bytes a
// power cut left of it; the Host can tell, as a real one would by its own
// framing (C-HOST-009), so reads end before it and the next append replaces
// it.
struct StoredBatch {
    uint64_t commit_seq;
    std::vector<uint8_t> bytes;
    bool torn = false;
};

enum class StoragePort {
    append,
    read_prefix,
    read_log,
    fence,
    write_snapshot,
    read_snapshot,
    trim_prefix,
    fence_writers = fence
};

// What a power cut leaves of an append still pending.
enum class CutAppend {
    lost,   // nothing
    stored, // the whole batch, though the kernel never learned it
    torn,   // its first half
};

enum class StorageFault {
    reject,            // the port returns AYNI_SUBMIT_REJECTED_NOT_RETAINED
    fail,              // completes with AYNI_WRITE_FAILED; nothing stored or read
    unknown_lost,      // completes with AYNI_WRITE_UNKNOWN; nothing stored or read
    unknown_persisted, // does the work, then completes with AYNI_WRITE_UNKNOWN
    short_write,       // keeps a torn append or partial snapshot, then FAILED
    unknown_deferred,  // copies an append; its owned bytes can arrive later
    accept_unknown,    // accepted request whose synchronous result is unknown
    unavailable,       // a writer fence cannot isolate its predecessor
};

struct StorageImage {
    std::vector<StoredBatch> log;
    std::array<std::vector<uint8_t>, 2> snapshots;
    uint32_t writer_epoch = 1;
};

// Serves one Kernel. The table's ctx points at this object, so it is neither
// copyable nor movable and must outlive the Kernel (C-HOST-001). Everything
// except wake and the ingress lock is for the owner thread.
class Host {
  public:
    explicit Host(uint64_t start_us = 0) : now_us_(start_us) {}
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    // Every port, storage included.
    ayni_host_v1 table();

    // FakeClock. Owner thread, like monotonic_now_us itself.
    uint64_t now_us() const { return now_us_; }
    void advance_us(uint64_t delta_us) { now_us_ += delta_us; }
    // From now on each monotonic_now_us call moves the clock by delta_us
    // after reading it; 0 turns this off.
    void advance_per_read_us(uint64_t delta_us) { advance_per_read_us_ = delta_us; }

    // Independent absolute clock; owner thread may change trust or jump UTC.
    void set_utc(int64_t seconds, ayni_utc_trust_t trust = AYNI_UTC_TRUSTED) {
        utc_s_ = seconds;
        utc_trust_ = trust;
        wake(this);
    }
    // Any thread.
    uint64_t wake_count() const { return wake_count_.load(); }
    // Host calls made by a thread that held the ingress lock at the time.
    uint64_t calls_under_lock() const { return calls_under_lock_.load(); }

    // The durable log, in append order; tests may preload or damage it.
    std::vector<StoredBatch> &log() { return storage_.log; }
    StorageImage &durable_storage() { return storage_; }
    void set_storage_capacity(uint64_t bytes) { storage_capacity_ = bytes; }
    uint32_t writer_epoch() const { return storage_.writer_epoch; }
    // Delayed backend work owns copies, never Kernel buffers. A successful
    // fence rejects these old-epoch writes even after their UNKNOWN post.
    std::size_t complete_late_storage();
    // The next call of port meets fault.
    void inject_fault(StoragePort port, StorageFault fault);
    std::size_t pending_storage() const { return pending_.size(); }
    // The attempt of every storage call so far, in call order.
    const std::vector<ayni_attempt_ref_v1> &storage_attempts() const { return attempts_; }
    // Completes every pending request in order and returns how many posts
    // the kernel accepted.
    std::size_t complete_storage();
    // Ends every pending request without posting it, as power loss ends the
    // kernel that made them: no later kernel sees their results (C-ID-006).
    // Each pending append leaves what append says. The kernel must not be
    // used again, and its slots never complete, so a test abandons its
    // memory instead of calling ayni_destroy.
    void power_cut(CutAppend append);

  private:
    struct Pending {
        StoragePort port;
        std::optional<StorageFault> fault;
        ayni_attempt_ref_v1 attempt;
        ayni_completion_t *completion;
        uint64_t commit_seq; // append: the batch's; read: from_commit_seq
        const uint8_t *data; // append: the encoded batch
        uint8_t *buffer;     // read: the kernel's buffer
        uint32_t size;       // encoded size, or buffer size
        uint32_t writer_epoch = 0;
        uint64_t transition_id = 0;
        uint32_t slot = 0;
    };

    static uint64_t monotonic_now_us(void *ctx);
    static void utc_now(void *ctx, ayni_utc_sample_v1 *out);
    static void wake(void *ctx);
    static void ingress_lock(void *ctx);
    static void ingress_unlock(void *ctx);
    static ayni_submit_result_t storage_append(void *ctx, const ayni_storage_batch_v1 *batch,
                                               ayni_attempt_ref_v1 attempt,
                                               ayni_completion_t *completion);
    static ayni_submit_result_t storage_read_prefix(void *ctx, uint64_t from_commit_seq,
                                                    uint8_t *buffer, uint32_t buffer_size,
                                                    ayni_attempt_ref_v1 attempt,
                                                    ayni_completion_t *completion);
    static ayni_submit_result_t storage_read_log(void *ctx, uint64_t cursor, uint8_t *buffer,
                                                 uint32_t buffer_size, ayni_attempt_ref_v1 attempt,
                                                 ayni_completion_t *completion);
    static ayni_submit_result_t storage_fence_writers(void *ctx, uint32_t minimum_epoch,
                                                      ayni_attempt_ref_v1 attempt,
                                                      ayni_completion_t *completion);
    static ayni_submit_result_t storage_write_snapshot(void *ctx, uint32_t writer_epoch,
                                                       uint32_t slot, uint64_t offset,
                                                       ayni_bytes_v1 bytes,
                                                       ayni_attempt_ref_v1 attempt,
                                                       ayni_completion_t *completion);
    static ayni_submit_result_t storage_read_snapshot(void *ctx, uint32_t slot, uint64_t offset,
                                                      uint8_t *buffer, uint32_t buffer_size,
                                                      ayni_attempt_ref_v1 attempt,
                                                      ayni_completion_t *completion);
    static ayni_submit_result_t storage_trim_prefix(void *ctx, uint32_t writer_epoch,
                                                    uint64_t transition_id, uint64_t commit_seq,
                                                    ayni_attempt_ref_v1 attempt,
                                                    ayni_completion_t *completion);
    static ayni_status_t storage_free_bytes(void *ctx, uint64_t *out_free_bytes);
    void note_call();
    ayni_submit_result_t accept(const Pending &request);
    ayni_completion_result_v1 run(const Pending &request);
    ayni_completion_result_v1 read(const Pending &request) const;
    ayni_completion_result_v1 snapshot(const Pending &request);
    uint64_t used_bytes() const;
    std::optional<StorageFault> next_fault(StoragePort port);

    uint64_t now_us_;
    int64_t utc_s_ = 0; // Simulated UTC seconds; advances only through set_utc.
    ayni_utc_trust_t utc_trust_ = AYNI_UTC_UNAVAILABLE; // No guessed time on startup.
    uint64_t advance_per_read_us_ = 0;
    std::atomic<uint64_t> wake_count_{0};
    std::atomic<uint64_t> calls_under_lock_{0};
    std::mutex ingress_;
    std::atomic<std::thread::id> holder_{};
    StorageImage storage_;
    uint64_t storage_capacity_ = 64U * 1024U * 1024U;
    std::deque<Pending> pending_;
    std::vector<ayni_attempt_ref_v1> attempts_;
    std::array<std::deque<StorageFault>, 7> faults_;
    struct LateAppend {
        uint32_t writer_epoch;
        StoredBatch batch;
    };
    std::vector<LateAppend> late_appends_;
};

} // namespace ayni::hosts::sim

#endif // AYNI_HOSTS_SIM_H
