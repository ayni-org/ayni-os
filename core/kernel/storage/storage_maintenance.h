// Publishes snapshots and compacts the log (C-STORE-006, C-STORE-010,
// C-STORE-011, ADR-0027 decisions 7 to 11).
//
// A run writes the work model, as of a durable boundary, into the slot not
// in use, then activates it. In order: a pass that only computes the payload
// CRC; the header; the payload a page at a time; reading the header and the
// payload back; the slot marker; the SNAPSHOT_COMMIT batch in the log, which
// is the switch; and trimming the log before that batch. Until the
// activation is stored, recovery still uses the old slot and the whole log;
// a power cut at any request leaves the work it had.
//
// The run borrows the storage buffer, so it starts only once the journal is
// empty and no transition starts until the run ends (ADR-0027 decision 4):
// the model it pages out does not change. One request is in flight at a
// time. Writing requests share the kernel's WriteSchedule and each has
// AppendRetry's bounded retries; a read that fails ends the run. A failed
// run fences the writer before reloading durable state.
//
// The class also keeps which slot and generation are in use, so it numbers
// each new snapshot: generation + 1, in the other slot.

#ifndef AYNI_CORE_KERNEL_STORAGE_STORAGE_MAINTENANCE_H
#define AYNI_CORE_KERNEL_STORAGE_STORAGE_MAINTENANCE_H

#include "durability/batch/batch_codec.h"
#include "durability/snapshot/snapshot_codec.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/storage/write_schedule.h"
#include "kernel/work_snapshot.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

// The durable state a snapshot is taken of.
struct SnapshotBoundary {
    uint64_t covers_commit_seq; // the last stored batch
    uint64_t max_transition_id; // the largest transition_id up to it
    uint64_t boot_epoch;        // the last boot epoch stored up to it
    uint32_t writer_epoch;      // the writer that stored it
};

class StorageMaintenance {
  public:
    // none releases the shared buffer; runtime owns it until end().
    enum class Mode { none, runtime };

    // host has every storage port, and it, completions, buffer, model and
    // writes outlive the maintenance. buffer is the kernel's storage buffer.
    StorageMaintenance(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
                       uint32_t capacity, WorkModel &model, WriteSchedule &writes);

    // The snapshot recovery found, or generation 0 when it found none.
    void adopt(uint64_t generation, uint32_t slot);
    uint64_t generation() const { return generation_; }
    // Another snapshot would need generation + 1, which does not exist.
    bool generation_exhausted() const;

    // Starts a run of the model as of boundary. Requests that write carry
    // writer_epoch, the writer this boot holds. Only
    // with mode() none and the storage buffer free.
    void start(const SnapshotBoundary &boundary, uint32_t writer_epoch);
    // none from end() until the next start(), while done() and failure()
    // still report the last run.
    Mode mode() const { return mode_; }
    // The kernel took the run's outcome, or a reload abandons it.
    void end() { mode_ = Mode::none; }

    // Owner thread. Takes the next step of the run: one CRC page, or one
    // storage request. Returns whether it did.
    bool advance(uint64_t now);
    // advance() would take a step at now, given a free completion slot.
    bool wants_advance(uint64_t now) const;
    // When the next step may be taken: 0 or a past time when it may be
    // taken now, the maximum while a request is in flight or no run is
    // active.
    uint64_t next_deadline() const;
    // A run has steps left.
    bool active() const;
    // The run stored its activation and trimmed the log.
    bool done() const;
    // Why the run failed; AYNI_RESTRICTION_NONE otherwise.
    ayni_restriction_t failure() const { return failure_; }

    // Of the last run: its slot header, the slot it wrote, and the identity
    // of its activation batch, after which the log continues.
    const durability::SnapshotMetadata &metadata() const { return metadata_; }
    uint32_t slot() const { return slot_; }
    durability::BatchIdentity activation_batch() const { return activation_batch_; }
    // What the log holds once trimmed: the activation batch alone.
    uint64_t retained_log_bytes() const { return durability::snapshot_commit_batch_size; }

  private:
    // The steps of a run in the order taken; done and failed end it.
    enum class Phase {
        idle,
        checksum,       // compute the payload CRC, a page per step
        header,         // write the slot header
        payload,        // write the payload, a page per request
        verify_header,  // read the header back
        verify_payload, // read the payload back and check its CRC
        marker,         // write the slot marker
        activation,     // append the SNAPSHOT_COMMIT batch
        trim,           // drop the log before the activation batch
        done,
        failed
    };
    static ayni_submit_result_t call(void *context, ayni_attempt_ref_v1 attempt,
                                     ayni_completion_t *completion);
    static void on_complete(void *context, const ayni_completion_result_v1 &result);
    ayni_submit_result_t port(ayni_attempt_ref_v1 attempt, ayni_completion_t *completion);
    void complete(const ayni_completion_result_v1 &result);
    // The phase's request may write the medium.
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
    Mode mode_ = Mode::none;
    WriteSchedule &writes_;
    WorkSnapshotWriter writer_;
    uint64_t generation_ = 0; // in use; 0 for none
    durability::SnapshotMetadata metadata_{};
    durability::BatchIdentity activation_batch_{};
    uint32_t active_slot_ = 0;
    uint32_t slot_ = 0; // the run's
    uint32_t writer_epoch_ = 0;
    bool in_flight_ = false;
    Phase phase_ = Phase::idle;
    AppendRetry retry_;
    // The slot offset of the next write while writing; the payload bytes
    // done while computing or checking the CRC.
    uint64_t offset_ = 0;
    uint32_t page_size_ = 0; // bytes in the buffer for the next request
    uint32_t crc_ = 0;       // of the payload so far
    ayni_restriction_t failure_ = AYNI_RESTRICTION_NONE;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_STORAGE_STORAGE_MAINTENANCE_H
