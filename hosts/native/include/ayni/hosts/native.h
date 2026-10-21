// Native Host for Linux and macOS builds: the platform services the Kernel
// uses to maintain its own invariants (ADR-0006): clock, wake and the ingress
// lock (ADR-0015), and the persistent log (C-HOST-009) in a file of a state
// directory once open_storage() succeeds.
//
// The storage ports only queue a request; one I/O thread owned by the Host
// serves the queue in order, reading or writing the kernel's buffer, and
// then posts the result with ayni_completion_post, after which it touches
// neither the buffer nor the completion (C-HOST-019). An append is OK only
// once the file is synced to the device (C-HOST-010). The file's framing
// and what a power cut leaves are described in hosts/native/file_log.h.

#ifndef AYNI_HOSTS_NATIVE_H
#define AYNI_HOSTS_NATIVE_H

#include <ayni/ayni.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ayni::hosts::native {

enum class StorageStatus {
    ok,
    invalid_argument, // no directory, or a request capacity of 0
    already_open,     // this Host's storage is open
    no_memory,
    in_use,       // another Host, in this process or another, has the log open
    not_a_log,    // the log file was not written in this format or version
    file_error,   // a file call failed; error is its errno
    thread_error, // the I/O thread did not start; error is the pthread code
};

struct StorageOpen {
    StorageStatus status;
    int error; // 0 unless status says otherwise
};

// Serves one Kernel. The table's ctx points at this object, so it is neither
// copyable nor movable and must outlive the Kernel (C-HOST-001).
class Host {
  public:
    // The log's file in the state directory.
    static constexpr const char *log_file_name = "commit_log";

    Host();
    // Closes the storage first.
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    // The clock is std::chrono::steady_clock, which never decreases. wake and
    // the ingress lock may be called from any thread. The storage ports are
    // in the table only while the storage is open, so a kernel created before
    // open_storage() has none and ayni_start returns AYNI_UNSUPPORTED
    // (C-HOST-015).
    ayni_host_v1 table();

    // For the owner thread: returns true as soon as wake has been called since
    // the previous return, or false once timeout passes without a wake.
    bool wait_for_wake(std::chrono::microseconds timeout);

    // Owner thread, before the kernel is created. Opens the log file in
    // directory, which must exist, creating the file on first use; checks
    // every stored frame, which reads the whole file; then starts the I/O
    // thread. request_capacity bounds the requests waiting for the thread; a
    // port called with that many waiting returns REJECTED_NOT_RETAINED, so it
    // should be at least the kernel's completion_slot_count. Nothing is open
    // unless the status is ok.
    StorageOpen open_storage(const char *directory, uint32_t request_capacity);

    // Owner thread. Completes every request already accepted, stops the I/O
    // thread and closes the file; storage ports called afterwards return
    // REJECTED_NOT_RETAINED. The composition root calls it once the kernel
    // is STOPPED, or was never started, and before ayni_destroy: in those
    // states no request is in flight, but the thread may still be returning
    // from its last ayni_completion_post (C-HOST-018). Does nothing when the
    // storage is not open.
    void close_storage();

  private:
    class Storage;

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

    std::mutex ingress_;
    std::mutex wake_mutex_;
    std::condition_variable wake_signal_;
    bool wake_pending_ = false;
    // Last, so that it is destroyed first: its thread may be posting, which
    // wakes through the members above.
    std::unique_ptr<Storage> storage_;
};

} // namespace ayni::hosts::native

#endif // AYNI_HOSTS_NATIVE_H
