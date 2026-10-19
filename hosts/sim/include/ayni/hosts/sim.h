// Simulation Host for tests and the simulator (MA-003, MA-005): time moves
// only when the test advances it (FakeClock, C-TIME-001), wakes are counted
// instead of scheduling a thread, and misuse of the ingress lock is recorded
// so that tests can assert the kernel never calls the Host while holding it
// (C-HOST-020). The lock is a real mutex, so producer threads may be used.
//
// Storage is an in-memory log. Requests stay pending until the test calls
// complete_storage(), which reads or fills the kernel's buffer only then, as
// a Host I/O thread would (C-HOST-019). Faults are injected per request.

#ifndef AYNI_HOSTS_SIM_H
#define AYNI_HOSTS_SIM_H

#include <ayni/ayni.h>

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
// append time, and the encoded bytes.
struct StoredBatch {
    uint64_t commit_seq;
    std::vector<uint8_t> bytes;
};

enum class StoragePort { append, read_prefix };

enum class StorageFault {
    reject,            // the port returns AYNI_SUBMIT_REJECTED_NOT_RETAINED
    fail,              // completes with AYNI_WRITE_FAILED; nothing stored or read
    unknown_lost,      // completes with AYNI_WRITE_UNKNOWN; nothing stored or read
    unknown_persisted, // does the work, then completes with AYNI_WRITE_UNKNOWN
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

    // Any thread.
    uint64_t wake_count() const { return wake_count_.load(); }
    // Host calls made by a thread that held the ingress lock at the time.
    uint64_t calls_under_lock() const { return calls_under_lock_.load(); }

    // The durable log, in append order; tests may preload or damage it.
    std::vector<StoredBatch> &log() { return log_; }
    // The next call of port meets fault.
    void inject_fault(StoragePort port, StorageFault fault);
    std::size_t pending_storage() const { return pending_.size(); }
    // Completes every pending request in order and returns how many posts
    // the kernel accepted.
    std::size_t complete_storage();

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
    };

    static uint64_t monotonic_now_us(void *ctx);
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
    void note_call();
    ayni_submit_result_t accept(const Pending &request);
    ayni_completion_result_v1 run(const Pending &request);
    ayni_completion_result_v1 read(const Pending &request) const;

    uint64_t now_us_;
    uint64_t advance_per_read_us_ = 0;
    std::atomic<uint64_t> wake_count_{0};
    std::atomic<uint64_t> calls_under_lock_{0};
    std::mutex ingress_;
    std::atomic<std::thread::id> holder_{};
    std::vector<StoredBatch> log_;
    std::deque<Pending> pending_;
    std::optional<StorageFault> append_fault_;
    std::optional<StorageFault> read_fault_;
};

} // namespace ayni::hosts::sim

#endif // AYNI_HOSTS_SIM_H
