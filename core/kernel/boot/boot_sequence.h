// Bounded asynchronous recovery, activation selection and writer isolation.
#ifndef AYNI_CORE_KERNEL_BOOT_BOOT_SEQUENCE_H
#define AYNI_CORE_KERNEL_BOOT_BOOT_SEQUENCE_H

#include "durability/batch/prefix_scan.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/storage/write_schedule.h"
#include "kernel/work_replay.h"
#include "kernel/work_snapshot.h"

#include <ayni/ayni.h>
#include <cstdint>

namespace ayni::kernel {

constexpr uint16_t boot_epoch_record_type = 1;
constexpr uint16_t boot_epoch_record_version = 1;
constexpr uint32_t boot_epoch_record_length = 8;
constexpr uint32_t activation_record_length = 32;

struct SnapshotActivation {
    uint32_t slot = 0;
    uint32_t payload_crc = 0;
    uint64_t generation = 0;
    uint64_t covers_commit_seq = 0;
    uint64_t max_transition_id = 0;
};
bool read_activation(const uint8_t *body, uint32_t size, SnapshotActivation &out);
void write_activation(const SnapshotActivation &activation, uint8_t *body);
bool activation_matches(const SnapshotActivation &activation,
                        const durability::SnapshotMetadata &metadata);

class BootSequence {
  public:
    enum class Outcome { pending, epoch_durable, migration_ready, writers_isolated, restricted };
    BootSequence(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
                 uint32_t buffer_size, WorkModel &model, WorkReplay &replay, WriteSchedule &writes);
    void reset(uint32_t isolated_epoch = 0);
    void isolate(uint32_t minimum_epoch);
    bool advance(uint64_t now_us);
    bool wants_advance(uint64_t now_us) const;
    uint64_t next_deadline() const;
    Outcome outcome() const;
    ayni_restriction_t restriction() const { return restriction_; }
    uint64_t boot_epoch() const { return boot_epoch_; }
    durability::BatchIdentity boot_batch() const { return boot_batch_; }
    uint64_t durable_seq() const { return known_durable_seq_; }
    uint64_t max_transition_id() const { return scan_.max_transition_id(); }
    uint32_t writer_epoch() const { return isolated_epoch_; }
    uint64_t last_boot_epoch() const { return last_boot_epoch_; }
    uint64_t generation() const { return selected_.generation; }
    uint32_t active_slot() const { return activation_.slot; }
    uint64_t log_bytes() const { return log_bytes_; }
    const durability::SnapshotMetadata &snapshot() const { return selected_; }
    uint32_t recovered_writer_epoch() const { return scan_.max_writer_epoch(); }

  private:
    enum class Phase {
        header,
        discover,
        marker,
        payload,
        replay,
        fence,
        writer_record,
        boot_record,
        pre_fence,
        isolated,
        done,
        migration,
        restricted
    };
    static ayni_submit_result_t call(void *context, ayni_attempt_ref_v1 attempt,
                                     ayni_completion_t *completion);
    static void on_complete(void *context, const ayni_completion_result_v1 &result);
    static durability::RecordUse on_record(void *context, const durability::BatchIdentity &batch,
                                           const durability::RecordHeader &header,
                                           const uint8_t *body);
    ayni_submit_result_t port(ayni_attempt_ref_v1 attempt, ayni_completion_t *completion);
    void complete(const ayni_completion_result_v1 &result);
    durability::RecordUse record(const durability::BatchIdentity &batch,
                                 const durability::RecordHeader &header, const uint8_t *body);
    bool read_log(const ayni_completion_result_v1 &result);
    bool scan_bytes(uint32_t size);
    void begin_replay();
    void after_replay();
    void prepare_append();
    bool mutates() const;
    void fail(ayni_restriction_t reason);
    void check(durability::BatchCheck result);

    const ayni_host_v1 &host_;
    CompletionPool &completions_;
    uint8_t *buffer_;
    uint32_t buffer_size_;
    WorkReplay &replay_;
    WriteSchedule &writes_;
    WorkSnapshotReader reader_;
    Phase phase_ = Phase::discover;
    bool in_flight_ = false;
    AppendRetry retry_;
    durability::SnapshotMetadata selected_{};
    SnapshotActivation activation_{};
    bool activation_seen_ = false;
    bool first_physical_ = true;
    bool legacy_ = false;
    uint64_t cursor_ = 0;
    uint64_t log_bytes_ = 0;
    uint64_t known_durable_seq_ = 0;
    uint64_t payload_read_ = 0;
    uint32_t carry_ = 0;
    uint32_t payload_crc_ = 0;
    durability::PrefixScan scan_{1};
    uint64_t last_boot_epoch_ = 0;
    uint64_t boot_epoch_ = 0;
    uint32_t isolated_epoch_ = 0;
    uint32_t fence_minimum_ = 1;
    uint32_t append_size_ = 0;
    durability::BatchIdentity boot_batch_{};
    ayni_restriction_t restriction_ = AYNI_RESTRICTION_NONE;
};
} // namespace ayni::kernel
#endif
