// ESP-IDF6.0.3 default legacy dual-core kernel (CONFIG_FREERTOS_SMP=n) Host.
// The optional upstream SMP kernel is explicitly refused until its join is validated.
// The platform services the Kernel uses to maintain its own
// invariants (ADR-0006): clock, wake and the ingress lock (ADR-0015), plus
// the boot-time platform report required by MA-001 and plan section 4.2.

#ifndef AYNI_HOSTS_ESP_IDF_H
#define AYNI_HOSTS_ESP_IDF_H

#include <ayni/ayni.h>
#include <ayni/host_clock.hpp>
#include <ayni/hosts/esp_idf/metrics.h>
#include <ayni/hosts/storage/file_value.h>
#include <cstddef>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace ayni::hosts::esp_idf {
// Source-level ESP Host status. file_error uses errno, not_a_volume reports raw lfs status.
enum class StorageStatus {
    ok,               // Complete storage ports and worker are available.
    invalid_argument, // Missing label or queue capacity outside 1..request_capacity_limit.
    already_open,     // This Host already owns its storage.
    no_memory,        // Internal-RAM Storage allocation failed.
    in_use,           // Another Ayni Host or existing VFS mount owns the partition.
    not_a_volume,     // Final raw LittleFS mount/format failed; error is that negative status.
    not_a_log,        // Existing Host log has an unsupported file/schema format.
    file_error,       // File/partition operation failed; error is an errno-compatible cause.
    task_error        // FreeRTOS did not create the statically backed worker.
};
struct StorageOpen {
    StorageStatus status; // Startup outcome; no partially opened storage is published.
    int error; // 0 without a cause; errno for file_error, raw lfs status for not_a_volume.
};

// Serves one Kernel. Construct it on the task that owns the Kernel: wake
// gives that task's default notification (index 0), which the owner's wait
// loop takes, so nothing else on that task may use index 0. The table's ctx
// points at this object, so it is neither copyable nor movable and must
// outlive the Kernel (C-HOST-001). The Host itself must be placed in internal RAM.
class Host {
  public:
    Host();
    // Owner-thread clock override; source/context remain valid through Host destruction.
    explicit Host(UtcSource utc);
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;

    // The clock is esp_timer, which counts microseconds since boot and never
    // decreases. wake and the ingress lock may be called from any task, never
    // from an ISR. The lock is a FreeRTOS mutex, so priority inheritance
    // bounds how long a low-priority poster can delay the owner task.
    ayni_host_v1 table();

    // Owner task before create. Dedicated LittleFS partition only; another Ayni Host
    // or existing VFS mount is refused. No external writer/mount may access the partition
    // during this lease. initialize_blank permits formatting entirely erased media only.
    // capacity is 1..16 queued requests; each accepted request completes exactly once.
    StorageOpen open_storage(const char *partition_label, uint32_t capacity,
                             bool initialize_blank = false);
    // Owner after STOPPED/before destroy. Drains accepted work, joins/deletes the parked
    // worker, then unmounts. May block on flash I/O. Captured old ports reject after close.
    void close_storage();
    // Owner-only outer data, same bounded queue/space budget and close barrier as Core storage.
    // Empty while closed; captured ports reject submissions after close.
    storage::FileValuePort value_store();
    // Owner wake uses notification index0; timeout is microseconds, rounded up to ticks.
    bool wait_for_wake(uint64_t timeout_us);
    // Last completed worker or current worker high-water free bytes; 0 before a worker ran.
    uint32_t worker_stack_free_bytes() const;
    // Owner only while open: bounded lock/copy, never performs I/O or resets counters.
    // A completion may be visible just before its accounting snapshot is published.
    // Close/reopen starts new counters; a closed Host returns a zero snapshot.
    StorageMetrics storage_metrics() const;
    // Startup accounting for one internal-RAM Storage including queue, caches and worker stack.
    static void storage_memory_requirements(std::size_t &bytes, std::size_t &alignment);
    static constexpr uint32_t worker_stack_bytes = 24 * 1024;
    static constexpr uint32_t request_capacity_limit = 16;

  private:
    class Storage;
    UtcSource utc_{}; // Immutable borrowed override; null sample uses the platform default.
    static ayni_submit_result_t submit_value(void *, const storage::FileValueRequest *);
    static ayni_submit_result_t storage_append_many(void *ctx, const ayni_storage_batch_v1 *,
                                                    uint32_t, ayni_attempt_ref_v1,
                                                    ayni_completion_t *);
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

    static void utc_now(void *ctx, ayni_utc_sample_v1 *out);
    static void wake(void *ctx);
    static void ingress_lock(void *ctx);
    static void ingress_unlock(void *ctx);

    StaticSemaphore_t ingress_storage_;
    SemaphoreHandle_t ingress_;
    TaskHandle_t owner_;
    uint32_t last_worker_stack_free_ = 0; // Retains joined worker evidence across close.
    Storage *storage_ = nullptr;          // Internal-RAM owner, destroyed after its worker joins.
};

// Logs chip model and revision, ESP-IDF version, reset reason, detected flash
// and PSRAM sizes, and the internal/PSRAM/DMA heap baseline (total, free,
// minimum free, largest free block) plus the calling task's stack high-water
// mark. Reads state only; safe to call from any task after boot.
void log_platform_baseline();

} // namespace ayni::hosts::esp_idf

#endif // AYNI_HOSTS_ESP_IDF_H
