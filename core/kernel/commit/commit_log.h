// The log writer of a RUNNING kernel (C-COMMIT-004, C-STORE-001, ADR-0026).
// Every transition encodes one batch into the journal ring in the storage
// buffer as it applies, and the log appends the queued batches to the Host
// in commit order, one request in flight at a time. durable_seq is the
// commit_seq of the last batch the Host stored; a batch above it can still
// be lost, and a later boot then never sees its transition.
//
// A transition needs room for the largest batch any transition writes
// before it starts, so encoding never fails halfway: with room, the kernel
// applies it; without, it waits until stored batches leave the ring.
//
// A failed or unknown append is retried with the same identity and bytes
// (AppendRetry, C-COMMIT-008), and no new transition starts until it is
// stored. When the attempts run out the writer ends, because a batch after a
// gap could never be read in order: the kernel then fences the writer and
// reloads (ADR-0027 decision 5). Appends share the kernel's WriteSchedule.

#ifndef AYNI_CORE_KERNEL_COMMIT_COMMIT_LOG_H
#define AYNI_CORE_KERNEL_COMMIT_COMMIT_LOG_H

#include "durability/batch/batch_codec.h"
#include "durability/journal/journal_ring.h"
#include "kernel/completion/completion_pool.h"
#include "kernel/storage/write_schedule.h"

#include <ayni/ayni.h>

#include <cstdint>

namespace ayni::kernel {

class CommitLog {
  public:
    // host has storage_append, and it, completions and buffer outlive the
    // log.
    CommitLog(const ayni_host_v1 &host, CompletionPool &completions, uint8_t *buffer,
              uint32_t buffer_size, WriteSchedule &writes);

    CommitLog(const CommitLog &) = delete;
    CommitLog &operator=(const CommitLog &) = delete;

    // Entering RUNNING, once recovery no longer uses the buffer, and again
    // after each snapshot activation, which used it: the next batch is
    // next_batch, the batches before it are durable, and no batch
    // is larger than batch_max, which fits the buffer with
    // JournalRing::entry_overhead. Requests carry boot_epoch.
    void start(uint64_t boot_epoch, const durability::BatchIdentity &next_batch,
               uint32_t batch_max);

    // Whether the next transition may start: the writer has not failed, no
    // append waits for a retry, and the ring holds a batch of batch_max
    // bytes in one piece.
    bool has_room() const;
    // Begins the batch of the next transition, which takes the next
    // identity; only when has_room().
    durability::BatchWriter begin();
    // Queues the batch begin() began, encoded_size bytes from finish().
    void queue(std::size_t encoded_size);

    // Owner thread. Appends the oldest queued batch when none is in flight
    // and returns whether it called the Host.
    bool advance(uint64_t now_us);
    // A batch is queued, none is in flight, the writer has not failed, and
    // the write interval and any retry backoff have passed at now_us.
    bool wants_advance(uint64_t now_us) const;
    // When the append that waits for the interval or a backoff may start;
    // the maximum when none waits, 0 or a past time when one may start now.
    uint64_t next_deadline() const;
    // An append failed and waits for its retry; no transition starts.
    bool paused() const { return retry_.retrying(); }
    // Nothing queued and nothing in flight.
    bool idle() const { return !in_flight_ && ring_.empty(); }

    // What start() was given: requests carry boot_epoch, and batches
    // writer_epoch.
    uint64_t boot_epoch() const { return boot_epoch_; }
    uint32_t writer_epoch() const { return writer_epoch_; }
    uint64_t durable_seq() const { return durable_seq_; }
    // Why the writer stopped; AYNI_RESTRICTION_NONE while it works.
    ayni_restriction_t failure() const { return failure_; }

  private:
    static ayni_submit_result_t call_append(void *context, ayni_attempt_ref_v1 attempt,
                                            ayni_completion_t *completion);
    static void on_append(void *context, const ayni_completion_result_v1 &result);
    void finish_append(const ayni_completion_result_v1 &result);
    // The identity of the batch with commit_seq.
    durability::BatchIdentity identity_of(uint64_t commit_seq) const;

    const ayni_host_v1 &host_;
    CompletionPool &completions_;
    WriteSchedule &writes_;
    AppendRetry retry_;
    durability::JournalRing ring_;
    uint64_t boot_epoch_ = 0;
    uint32_t writer_epoch_ = 0;
    // transition_id - commit_seq, modulo 2^64, the same for every batch of a
    // boot: each transition takes the next of both.
    uint64_t transition_offset_ = 0;
    uint64_t next_seq_ = 0;    // of the batch begin() begins next
    uint64_t durable_seq_ = 0; // of the last batch stored
    uint32_t batch_max_ = 0;
    bool in_flight_ = false;
    ayni_restriction_t failure_ = AYNI_RESTRICTION_NONE;
};

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMIT_COMMIT_LOG_H
