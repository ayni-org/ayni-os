// Snapshot publication owns the storage buffer only after the journal drains.
#ifndef AYNI_CORE_KERNEL_STORAGE_MAINTENANCE_H
#define AYNI_CORE_KERNEL_STORAGE_MAINTENANCE_H

#include "kernel/boot/boot_sequence.h"

namespace ayni::kernel {
class StorageMaintenance {
  public:
    StorageMaintenance(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
                       uint32_t capacity, WorkModel &model, WriteSchedule &writes);
    void start(durability::SnapshotMetadata metadata, uint32_t slot, uint32_t writer_epoch,
               bool migration);
    bool advance(uint64_t now);
    bool wants_advance(uint64_t now) const;
    uint64_t next_deadline() const;
    bool active() const;
    bool done() const;
    ayni_restriction_t failure() const { return failure_; }
    const durability::SnapshotMetadata &metadata() const { return metadata_; }
    durability::BatchIdentity activation_batch() const { return activation_batch_; }
    uint32_t slot() const { return slot_; }
    uint64_t retained_log_bytes() const {
        return durability::batch_size(activation_record_length, 1);
    }

  private:
    enum class Phase {
        idle,
        checksum,
        header,
        payload,
        verify_header,
        verify_payload,
        marker,
        activation,
        trim,
        done,
        failed
    };
    static ayni_submit_result_t call(void *context, ayni_attempt_ref_v1 attempt,
                                     ayni_completion_t *completion);
    static void on_complete(void *context, const ayni_completion_result_v1 &result);
    ayni_submit_result_t port(ayni_attempt_ref_v1 attempt, ayni_completion_t *completion);
    void complete(const ayni_completion_result_v1 &result);
    bool mutates() const;
    void fail(ayni_restriction_t reason);
    void prepare_header();
    void prepare_payload();
    void prepare_marker();
    void prepare_activation();

    const ayni_host_v1 &host_;
    CompletionPool &completions_;
    uint8_t *buffer_;
    uint32_t capacity_;
    WriteSchedule &writes_;
    WorkSnapshotWriter writer_;
    durability::SnapshotMetadata metadata_{};
    durability::BatchIdentity activation_batch_{};
    uint32_t slot_ = 0;
    uint32_t writer_epoch_ = 0;
    bool migration_ = false;
    bool in_flight_ = false;
    Phase phase_ = Phase::idle;
    AppendRetry retry_;
    uint64_t offset_ = 0;
    uint32_t page_size_ = 0;
    uint32_t crc_ = 0;
    ayni_restriction_t failure_ = AYNI_RESTRICTION_NONE;
};
} // namespace ayni::kernel
#endif
