// Recovery from ayni_start up to RUNNING (ADR-0016, ADR-0027). It reads
// what is durable, fences the writers before this boot, and persists this
// boot's epoch in a batch of its own before the kernel opens any output
// (C-ID-009).
//
// The log is read twice, by physical cursor (ADR-0027 decision 6). The
// first pass, discovery, checks every batch and finds the latest
// SNAPSHOT_COMMIT. When there is one, its slot comes next: the header, which
// must match the activation, the marker, and the payload a page at a time,
// whose records restore the work model (work_snapshot.h). The second pass
// replays the batches the snapshot does not cover, handing every work record
// to WorkReplay (ADR-0026). Then the Host fences the writers before this
// boot and installs its writer epoch, and two batches of that writer follow:
// WRITER_FENCE, then BOOT_EPOCH.
//
// A schema 0 log is read the same way, but once its writers are fenced the
// sequence stops at migration_ready: the kernel writes the model as a schema
// 1 snapshot and starts recovery over from it (ADR-0027 decision 11). To
// fence a RUNNING writer the kernel calls isolate() instead, which only
// fences; the reload after it resets the sequence with the epoch installed.
//
// One storage request is in flight at a time, so the kernel's storage buffer
// serves every read and append. Requests use boot_epoch 0: this boot's epoch
// is not durable until its batch is stored (C-ID-006). Fences and appends
// share the kernel's WriteSchedule; an append that fails is retried
// (AppendRetry), a fence that fails restricts (ADR-0027 decision 5).

#ifndef AYNI_CORE_KERNEL_BOOT_BOOT_SEQUENCE_H
#define AYNI_CORE_KERNEL_BOOT_BOOT_SEQUENCE_H

#include "durability/batch/prefix_scan.h"
#include "durability/snapshot/snapshot_codec.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/storage/write_schedule.h"
#include "kernel/work_replay.h"
#include "kernel/work_snapshot.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// BOOT_EPOCH (contracts/storage/records.md): a REQUIRED record whose body is
// the epoch as a little-endian uint64.
constexpr uint16_t boot_epoch_record_type = 1;
constexpr uint16_t boot_epoch_record_version = 1;
constexpr uint32_t boot_epoch_record_length = 8;

class BootSequence {
  public:
    enum class Outcome {
        pending,
        epoch_durable,    // RUNNING may start; boot_epoch() is set
        migration_ready,  // a schema 0 log is read and its writers fenced
        writers_isolated, // isolate() installed writer_epoch()
        restricted        // restriction() says why
    };

    // host has every storage port, and it, completions, buffer, model,
    // replay and writes outlive the sequence. buffer_size is within the
    // ayni.h storage buffer range.
    BootSequence(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
                 uint32_t buffer_size, WorkModel &model, WorkReplay &replay, WriteSchedule &writes);

    BootSequence(const BootSequence &) = delete;
    BootSequence &operator=(const BootSequence &) = delete;

    // Starts recovery over, with no request in flight. A reload passes the
    // writer epoch its fence installed: recovery then does not fence again,
    // and its batches carry that epoch. 0 fences anew.
    void reset(uint32_t isolated_epoch = 0);
    // Only fences, for a RUNNING writer that failed (ADR-0027 decision 5):
    // once the Host installs a writer epoch of at least minimum_epoch, the
    // outcome is writers_isolated.
    void isolate(uint32_t minimum_epoch);

    // Owner thread. Issues the next storage request when one is due and
    // returns whether it called the Host.
    bool advance(uint64_t now_us);
    // A request is due at now_us, given a free completion slot.
    bool wants_advance(uint64_t now_us) const;
    // When the next request may start: 0 for a read, the end of the write
    // interval and any retry backoff for a write; the maximum while a
    // request is in flight or none follows.
    uint64_t next_deadline() const;

    Outcome outcome() const;
    ayni_restriction_t restriction() const { return restriction_; }
    // 0 until the BOOT_EPOCH batch is durable; then at most
    // foundation::max_boot_epoch, so that kernel ids can embed it.
    uint64_t boot_epoch() const { return boot_epoch_; }
    // The batch that stored this boot's epoch, once outcome() is
    // epoch_durable; transitions after it take the following ids.
    durability::BatchIdentity boot_batch() const { return boot_batch_; }
    // The last commit_seq known durable: the end of the log read so far,
    // then each batch this boot stored.
    uint64_t durable_seq() const { return known_durable_seq_; }
    // The largest transition_id and writer_epoch of the batches read and
    // stored so far.
    uint64_t max_transition_id() const { return scan_.max_transition_id(); }
    uint32_t recovered_writer_epoch() const { return scan_.max_writer_epoch(); }
    // The writer epoch the fence installed, which this boot's batches carry.
    uint32_t writer_epoch() const { return isolated_epoch_; }
    // The largest BOOT_EPOCH read or stored so far.
    uint64_t last_boot_epoch() const { return last_boot_epoch_; }
    // The snapshot recovery started from: generation 0 for none.
    uint64_t generation() const { return selected_.generation; }
    uint32_t active_slot() const { return activation_.slot; }
    // The bytes the log holds: those discovery read and the batches this
    // boot stored.
    uint64_t log_bytes() const { return log_bytes_; }

  private:
    // The requests in the order they are issued.
    enum class Phase {
        discover,      // read the log, checking it, for the latest activation
        header,        // read the activated slot's header
        marker,        // read its marker
        payload,       // read its payload, a page per request
        replay,        // read the log again, replaying what follows the snapshot
        fence,         // fence the writers before this boot
        writer_record, // append WRITER_FENCE
        boot_record,   // append BOOT_EPOCH
        done,
        pre_fence, // isolate()'s fence
        isolated,
        migration, // the kernel migrates a schema 0 log
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
    void fenced(const ayni_completion_result_v1 &result);
    void appended(const ayni_completion_result_v1 &result);
    void read(const ayni_completion_result_v1 &result);
    void read_payload(uint32_t size);
    void read_log(const ayni_completion_result_v1 &result);
    void scan_bytes(uint32_t size);
    bool start_after_trim(const durability::BatchView &batch);
    durability::RecordUse record(const durability::BatchIdentity &batch,
                                 const durability::RecordHeader &header, const uint8_t *body);
    void begin_replay();
    void after_replay();
    // The identity of the batch this boot appends next.
    durability::BatchIdentity next_batch() const;
    void prepare_append();
    // No request follows until the kernel acts: the sequence is done or
    // failed, or waits for a migration or a reload.
    bool stopped() const;
    // The phase's request may write the medium.
    bool mutates() const;
    void fail(ayni_restriction_t reason);
    void check(durability::BatchCheck result);

    const ayni_host_v1 &host_;
    CompletionPool &completions_;
    uint8_t *buffer_;
    uint32_t buffer_size_;
    WorkReplay &replay_;
    WriteSchedule &writes_;
    WorkSnapshotReader reader_; // restores the payload into the model
    Phase phase_ = Phase::discover;
    bool in_flight_ = false;
    AppendRetry retry_;
    // The selected slot's header, and the latest activation discovery found.
    durability::SnapshotMetadata selected_{};
    durability::SnapshotActivation activation_{};
    bool activation_seen_ = false;
    bool first_physical_ = true; // no batch has been read yet
    bool legacy_ = false;        // the log has a schema 0 batch
    uint64_t cursor_ = 0;        // where the next log read starts
    uint64_t log_bytes_ = 0;
    uint64_t known_durable_seq_ = 0;
    // Of the payload: the bytes read, those of a record the last page cut,
    // kept at the start of the buffer, and the CRC so far.
    uint64_t payload_read_ = 0;
    uint32_t carry_ = 0;
    uint32_t payload_crc_ = 0;
    durability::PrefixScan scan_{1};
    uint64_t last_boot_epoch_ = 0;
    uint64_t boot_epoch_ = 0;
    uint32_t isolated_epoch_ = 0;
    uint32_t fence_minimum_ = 1; // isolate()'s
    uint32_t append_size_ = 0;   // of the batch in the buffer
    durability::BatchIdentity boot_batch_{};
    ayni_restriction_t restriction_ = AYNI_RESTRICTION_NONE;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_BOOT_BOOT_SEQUENCE_H
